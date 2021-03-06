/*
 * Astra Module: MPEG-TS (MPTS Demux)
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2014, Andrey Dyldin <and@cesbo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Module Name:
 *      channel
 *
 * Module Role:
 *      Input stage, requests pids
 *
 * Module Options:
 *      upstream    - object, stream instance returned by module_instance:stream()
 *      name        - string, channel name
 *      pnr         - number, join PID related to the program number
 *      pid         - list, join PID in list
 *      no_sdt      - boolean, do not join SDT table
 *      no_eit      - boolean, do not join EIT table
 *      cas         - boolean, join CAT, ECM, EMM tables
 *      set_pnr     - number, replace original PNR
 *      map         - list, map PID by stream type, item format: "type=pid"
 *                    type: video, audio, rus, eng... and other languages code
 *                     pid: number identifier in range 32-8190
 *      filter      - list, drop PID
 */

#include <astra/astra.h>
#include <astra/core/list.h>
#include <astra/core/timer.h>
#include <astra/luaapi/stream.h>
#include <astra/mpegts/psi.h>

typedef struct
{
    char type[6];
    uint16_t origin_pid;
    uint16_t custom_pid;
    bool is_set;
} map_item_t;

struct module_data_t
{
    STREAM_MODULE_DATA();

    /* Options */
    struct
    {
        const char *name;
        int pnr;
        int set_pnr;
        bool no_sdt;
        bool no_eit;
        bool no_reload;
        bool cas;

        bool pass_sdt;
        bool pass_eit;
    } config;

    /* */
    asc_list_t *map;
    uint16_t pid_map[TS_MAX_PIDS];
    uint8_t custom_ts[TS_PACKET_SIZE];

    ts_psi_t *pat;
    ts_psi_t *cat;
    ts_psi_t *pmt;
    ts_psi_t *sdt;
    ts_psi_t *eit;

    ts_type_t stream[TS_MAX_PIDS];

    uint16_t tsid;
    ts_psi_t *custom_pat;
    ts_psi_t *custom_cat;
    ts_psi_t *custom_pmt;
    ts_psi_t *custom_sdt;

    /* */
    uint8_t sdt_original_section_id;
    uint8_t sdt_max_section_id;
    uint32_t *sdt_checksum_list;

    uint8_t eit_cc;

    uint8_t pat_version;
    asc_timer_t *si_timer;
};

#define MSG(_msg) "[channel %s] " _msg, mod->config.name

static void stream_reload(module_data_t *mod)
{
    memset(mod->stream, 0, sizeof(mod->stream));

    for(int __i = 0; __i < TS_MAX_PIDS; ++__i)
    {
        if(module_demux_check(mod, __i))
            module_demux_leave(mod, __i);
    }

    mod->pat->crc32 = 0;
    mod->pmt->crc32 = 0;

    mod->stream[0x00] = TS_TYPE_PAT;
    module_demux_join(mod, 0x00);

    if(mod->config.cas)
    {
        mod->cat->crc32 = 0;
        mod->stream[0x01] = TS_TYPE_CAT;
        module_demux_join(mod, 0x01);
    }

    if(mod->config.no_sdt == false)
    {
        mod->stream[0x11] = TS_TYPE_SDT;
        module_demux_join(mod, 0x11);
        if(mod->sdt_checksum_list)
        {
            free(mod->sdt_checksum_list);
            mod->sdt_checksum_list = NULL;
        }
    }

    if(mod->config.no_eit == false)
    {
        mod->stream[0x12] = TS_TYPE_EIT;
        module_demux_join(mod, 0x12);

        mod->stream[0x14] = TS_TYPE_TDT;
        module_demux_join(mod, 0x14);
    }

    if(mod->map)
    {
        asc_list_for(mod->map)
        {
            map_item_t *map_item = (map_item_t *)asc_list_data(mod->map);
            map_item->is_set = false;
        }
    }
}

static void on_si_timer(void *arg)
{
    module_data_t *mod = (module_data_t *)arg;

    if(mod->custom_pat)
        ts_psi_demux(mod->custom_pat, module_stream_send, mod);

    if(mod->custom_cat)
        ts_psi_demux(mod->custom_cat, module_stream_send, mod);

    if(mod->custom_pmt)
        ts_psi_demux(mod->custom_pmt, module_stream_send, mod);

    if(mod->custom_sdt)
        ts_psi_demux(mod->custom_sdt, module_stream_send, mod);
}

/*
 * oooooooooo   o   ooooooooooo
 *  888    888 888  88  888  88
 *  888oooo88 8  88     888
 *  888      8oooo88    888
 * o888o   o88o  o888o o888o
 *
 */

static void on_pat(void *arg, ts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x00)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        ts_psi_demux(mod->custom_pat, module_stream_send, mod);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("PAT checksum error"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("PAT changed. Reload stream info"));
        stream_reload(mod);
    }

    psi->crc32 = crc32;

    mod->tsid = PAT_GET_TSID(psi);

    const uint8_t *pointer;

    PAT_ITEMS_FOREACH(psi, pointer)
    {
        const uint16_t pnr = PAT_ITEM_GET_PNR(psi, pointer);
        if(!pnr)
            continue;

        if(!mod->config.pnr)
            mod->config.pnr = pnr;

        if(pnr == mod->config.pnr)
        {
            const uint16_t pid = PAT_ITEM_GET_PID(psi, pointer);
            mod->stream[pid] = TS_TYPE_PMT;
            module_demux_join(mod, pid);
            mod->pmt->pid = pid;
            mod->pmt->crc32 = 0;
            break;
        }
    }

    if(PAT_ITEMS_EOL(psi, pointer))
    {
        mod->custom_pat->buffer_size = 0;
        asc_log_error(MSG("PAT: stream with id %d is not found"), mod->config.pnr);
        return;
    }

    const uint8_t pat_version = PAT_GET_VERSION(mod->custom_pat) + 1;
    PAT_INIT(mod->custom_pat, mod->tsid, pat_version);
    memcpy(PAT_ITEMS_FIRST(mod->custom_pat), pointer, 4);

    mod->custom_pmt->pid = mod->pmt->pid;

    if(mod->config.set_pnr)
    {
        uint8_t *custom_pointer = PAT_ITEMS_FIRST(mod->custom_pat);
        PAT_ITEM_SET_PNR(mod->custom_pat, custom_pointer, mod->config.set_pnr);
    }

    if(mod->map)
    {
        asc_list_for(mod->map)
        {
            map_item_t *map_item = (map_item_t *)asc_list_data(mod->map);
            if(map_item->is_set)
                continue;

            if(   (map_item->origin_pid && map_item->origin_pid == mod->pmt->pid)
               || (!strcmp(map_item->type, "pmt")) )
            {
                map_item->is_set = true;
                mod->pid_map[mod->pmt->pid] = map_item->custom_pid;

                uint8_t *custom_pointer = PAT_ITEMS_FIRST(mod->custom_pat);
                PAT_ITEM_SET_PID(mod->custom_pat, custom_pointer, map_item->custom_pid);

                mod->custom_pmt->pid = map_item->custom_pid;
                break;
            }
        }
    }

    mod->pat_version = (mod->pat_version + 1) & 0x0F;
    PAT_SET_VERSION(mod->custom_pat, mod->pat_version);

    mod->custom_pat->buffer_size = 8 + 4 + CRC32_SIZE;
    PSI_SET_SIZE(mod->custom_pat);
    PSI_SET_CRC32(mod->custom_pat);

    ts_psi_demux(mod->custom_pat, module_stream_send, mod);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = TS_TYPE_UNKNOWN;
}

/*
 *   oooooooo8     o   ooooooooooo
 * o888     88    888  88  888  88
 * 888           8  88     888
 * 888o     oo  8oooo88    888
 *  888oooo88 o88o  o888o o888o
 *
 */

static void on_cat(void *arg, ts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x01)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        ts_psi_demux(mod->custom_cat, module_stream_send, mod);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("CAT checksum error"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("CAT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    psi->crc32 = crc32;

    const uint8_t *desc_pointer;

    CAT_DESC_FOREACH(psi, desc_pointer)
    {
        if(desc_pointer[0] == 0x09)
        {
            const uint16_t ca_pid = DESC_CA_PID(desc_pointer);
            if(mod->stream[ca_pid] == TS_TYPE_UNKNOWN && ca_pid != TS_NULL_PID)
            {
                mod->stream[ca_pid] = TS_TYPE_CA;
                if(mod->pid_map[ca_pid] == TS_MAX_PIDS)
                    mod->pid_map[ca_pid] = 0;
                module_demux_join(mod, ca_pid);
            }
        }
    }

    memcpy(mod->custom_cat->buffer, psi->buffer, psi->buffer_size);
    mod->custom_cat->buffer_size = psi->buffer_size;
    mod->custom_cat->cc = 0;

    ts_psi_demux(mod->custom_cat, module_stream_send, mod);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = TS_TYPE_UNKNOWN;
}

/*
 * oooooooooo oooo     oooo ooooooooooo
 *  888    888 8888o   888  88  888  88
 *  888oooo88  88 888o8 88      888
 *  888        88  888  88      888
 * o888o      o88o  8  o88o    o888o
 *
 */

static uint16_t map_custom_pid(module_data_t *mod, uint16_t pid, const char *type)
{
    asc_list_for(mod->map)
    {
        map_item_t *map_item = (map_item_t *)asc_list_data(mod->map);
        if(map_item->is_set)
            continue;

        if(   (map_item->origin_pid && map_item->origin_pid == pid)
           || (!strcmp(map_item->type, type)) )
        {
            map_item->is_set = true;
            mod->pid_map[pid] = map_item->custom_pid;

            return map_item->custom_pid;
        }
    }
    return 0;
}

static void on_pmt(void *arg, ts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x02)
        return;

    // check pnr
    const uint16_t pnr = PMT_GET_PNR(psi);
    if(pnr != mod->config.pnr)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        ts_psi_demux(mod->custom_pmt, module_stream_send, mod);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("PMT checksum error"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("PMT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    psi->crc32 = crc32;

    uint16_t skip = 12;
    memcpy(mod->custom_pmt->buffer, psi->buffer, 10);

    const uint16_t pcr_pid = PMT_GET_PCR(psi);
    bool join_pcr = true;

    const uint8_t *desc_pointer;

    PMT_DESC_FOREACH(psi, desc_pointer)
    {
        if(desc_pointer[0] == 0x09)
        {
            if(!mod->config.cas)
                continue;

            const uint16_t ca_pid = DESC_CA_PID(desc_pointer);
            if(mod->stream[ca_pid] == TS_TYPE_UNKNOWN && ca_pid != TS_NULL_PID)
            {
                mod->stream[ca_pid] = TS_TYPE_CA;
                if(mod->pid_map[ca_pid] == TS_MAX_PIDS)
                    mod->pid_map[ca_pid] = 0;
                module_demux_join(mod, ca_pid);
            }
        }

        const uint8_t size = desc_pointer[1] + 2;
        memcpy(&mod->custom_pmt->buffer[skip], desc_pointer, size);
        skip += size;
    }

    {
        const uint16_t size = skip - 12; // 12 - PMT header
        mod->custom_pmt->buffer[10] = (psi->buffer[10] & 0xF0) | ((size >> 8) & 0x0F);
        mod->custom_pmt->buffer[11] = (size & 0xFF);
    }

    if(mod->config.set_pnr)
    {
        PMT_SET_PNR(mod->custom_pmt, mod->config.set_pnr);
    }

    const uint8_t *pointer;
    PMT_ITEMS_FOREACH(psi, pointer)
    {
        const uint16_t pid = PMT_ITEM_GET_PID(psi, pointer);

        if(mod->pid_map[pid] == TS_MAX_PIDS) // skip filtered pid
            continue;

        const uint8_t item_type = PMT_ITEM_GET_TYPE(psi, pointer);
        const ts_stream_type_t *const st = ts_stream_type(item_type);
        ts_type_t ts_type = st->pkt_type;
        const uint8_t *language_desc = NULL;

        const uint16_t skip_last = skip;

        memcpy(&mod->custom_pmt->buffer[skip], pointer, 5);
        skip += 5;

        mod->stream[pid] = TS_TYPE_PES;
        module_demux_join(mod, pid);

        if(pid == pcr_pid)
            join_pcr = false;

        PMT_ITEM_DESC_FOREACH(pointer, desc_pointer)
        {
            const uint8_t desc_type = desc_pointer[0];

            if(desc_type == 0x09)
            {
                if(!mod->config.cas)
                    continue;

                const uint16_t ca_pid = DESC_CA_PID(desc_pointer);
                if(mod->stream[ca_pid] == TS_TYPE_UNKNOWN && ca_pid != TS_NULL_PID)
                {
                    mod->stream[ca_pid] = TS_TYPE_CA;
                    if(mod->pid_map[ca_pid] == TS_MAX_PIDS)
                        mod->pid_map[ca_pid] = 0;
                    module_demux_join(mod, ca_pid);
                }
            }
            else if(desc_type == 0x0A)
            {
                language_desc = desc_pointer;
            }
            else if(item_type == 0x06 && ts_type == TS_TYPE_DATA)
            {
                ts_type = ts_priv_type(desc_type);
            }

            const uint8_t size = desc_pointer[1] + 2;
            memcpy(&mod->custom_pmt->buffer[skip], desc_pointer, size);
            skip += size;
        }

        {
            const uint16_t size = skip - skip_last - 5;
            mod->custom_pmt->buffer[skip_last + 3] = (size << 8) & 0x0F;
            mod->custom_pmt->buffer[skip_last + 4] = (size & 0xFF);
        }

        if(mod->map)
        {
            uint16_t custom_pid = 0;

            switch(ts_type)
            {
                case TS_TYPE_VIDEO:
                {
                    custom_pid = map_custom_pid(mod, pid, "video");
                    break;
                }
                case TS_TYPE_AUDIO:
                {
                    if(language_desc)
                    {
                        char lang[4];
                        lang[0] = language_desc[2];
                        lang[1] = language_desc[3];
                        lang[2] = language_desc[4];
                        lang[3] = 0;
                        custom_pid = map_custom_pid(mod, pid, lang);
                    }
                    if(!custom_pid)
                        custom_pid = map_custom_pid(mod, pid, "audio");
                    break;
                }
                case TS_TYPE_SUB:
                {
                    custom_pid = map_custom_pid(mod, pid, "sub");
                    break;
                }
                default:
                {
                    custom_pid = map_custom_pid(mod, pid, "");
                    break;
                }
            }

            if(custom_pid)
            {
                PMT_ITEM_SET_PID(  mod->custom_pmt
                                 , &mod->custom_pmt->buffer[skip_last]
                                 , custom_pid);
            }
        }
    }
    mod->custom_pmt->buffer_size = skip + CRC32_SIZE;

    if(join_pcr)
    {
        mod->stream[pcr_pid] = TS_TYPE_PES;
        if(mod->pid_map[pcr_pid] == TS_MAX_PIDS)
            mod->pid_map[pcr_pid] = 0;
        module_demux_join(mod, pcr_pid);
    }

    if(mod->map)
    {
        if(mod->pid_map[pcr_pid])
            PMT_SET_PCR(mod->custom_pmt, mod->pid_map[pcr_pid]);
    }

    PSI_SET_SIZE(mod->custom_pmt);
    PSI_SET_CRC32(mod->custom_pmt);
    ts_psi_demux(mod->custom_pmt, module_stream_send, mod);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = TS_TYPE_UNKNOWN;
}

/*
 *  oooooooo8 ooooooooo   ooooooooooo
 * 888         888    88o 88  888  88
 *  888oooooo  888    888     888
 *         888 888    888     888
 * o88oooo888 o888ooo88      o888o
 *
 */

static void on_sdt(void *arg, ts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x42)
        return;

    if(mod->tsid != SDT_GET_TSID(psi))
        return;

    const uint32_t crc32 = PSI_GET_CRC32(psi);

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("SDT checksum error"));
        return;
    }

    // check changes
    if(!mod->sdt_checksum_list)
    {
        const uint8_t max_section_id = SDT_GET_LAST_SECTION_NUMBER(psi);
        mod->sdt_max_section_id = max_section_id;
        mod->sdt_checksum_list = ASC_ALLOC(max_section_id + 1, uint32_t);
    }
    const uint8_t section_id = SDT_GET_SECTION_NUMBER(psi);
    if(section_id > mod->sdt_max_section_id)
    {
        asc_log_warning(MSG("SDT: section_number is greater then section_last_number"));
        return;
    }
    if(mod->sdt_checksum_list[section_id] == crc32)
    {
        if(mod->sdt_original_section_id == section_id)
            ts_psi_demux(mod->custom_sdt, module_stream_send, mod);

        return;
    }

    if(mod->sdt_checksum_list[section_id] != 0)
    {
        asc_log_warning(MSG("SDT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    mod->sdt_checksum_list[section_id] = crc32;

    const uint8_t *pointer;
    SDT_ITEMS_FOREACH(psi, pointer)
    {
        if(SDT_ITEM_GET_SID(psi, pointer) == mod->config.pnr)
            break;
    }

    if(SDT_ITEMS_EOL(psi, pointer))
        return;

    mod->sdt_original_section_id = section_id;

    memcpy(mod->custom_sdt->buffer, psi->buffer, 11); // copy SDT header
    SDT_SET_SECTION_NUMBER(mod->custom_sdt, 0);
    SDT_SET_LAST_SECTION_NUMBER(mod->custom_sdt, 0);

    const uint16_t item_length = __SDT_ITEM_DESC_SIZE(pointer) + 5;
    memcpy(&mod->custom_sdt->buffer[11], pointer, item_length);
    const uint16_t section_length = item_length + 8 + CRC32_SIZE;
    mod->custom_sdt->buffer_size = 3 + section_length;

    if(mod->config.set_pnr)
    {
        uint8_t *custom_pointer = SDT_ITEMS_FIRST(mod->custom_sdt);
        SDT_ITEM_SET_SID(mod->custom_sdt, custom_pointer, mod->config.set_pnr);
    }

    PSI_SET_SIZE(mod->custom_sdt);
    PSI_SET_CRC32(mod->custom_sdt);

    ts_psi_demux(mod->custom_sdt, module_stream_send, mod);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = TS_TYPE_UNKNOWN;
}

/*
 * ooooooooooo ooooo ooooooooooo
 *  888    88   888  88  888  88
 *  888ooo8     888      888
 *  888    oo   888      888
 * o888ooo8888 o888o    o888o
 *
 */

static void on_eit(void *arg, ts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    const uint8_t table_id = psi->buffer[0];
    const bool is_actual_eit = (table_id == 0x4E || (table_id >= 0x50 && table_id <= 0x5F));
    if(!is_actual_eit)
        return;

    if(mod->tsid != EIT_GET_TSID(psi))
        return;

    if(mod->config.pnr != EIT_GET_PNR(psi))
        return;

    psi->cc = mod->eit_cc;

    if(mod->config.set_pnr)
    {
        EIT_SET_PNR(psi, mod->config.set_pnr);
        PSI_SET_CRC32(psi);
    }

    ts_psi_demux(psi, module_stream_send, mod);

    mod->eit_cc = psi->cc;
}

/*
 * ooooooooooo  oooooooo8
 * 88  888  88 888
 *     888      888oooooo
 *     888             888
 *    o888o    o88oooo888
 *
 */

static void on_ts(module_data_t *mod, const uint8_t *ts)
{
    const uint16_t pid = TS_GET_PID(ts);
    if(!module_demux_check(mod, pid))
        return;

    if(pid == TS_NULL_PID)
        return;

    switch(mod->stream[pid])
    {
        case TS_TYPE_PES:
            break;
        case TS_TYPE_PAT:
            ts_psi_mux(mod->pat, ts, on_pat, mod);
            return;
        case TS_TYPE_CAT:
            ts_psi_mux(mod->cat, ts, on_cat, mod);
            return;
        case TS_TYPE_PMT:
            ts_psi_mux(mod->pmt, ts, on_pmt, mod);
            return;
        case TS_TYPE_SDT:
            if(mod->config.pass_sdt)
                break;
            ts_psi_mux(mod->sdt, ts, on_sdt, mod);
            return;
        case TS_TYPE_EIT:
            if(mod->config.pass_eit)
                break;
            ts_psi_mux(mod->eit, ts, on_eit, mod);
            return;
        case TS_TYPE_UNKNOWN:
            return;
        default:
            break;
    }

    if(mod->pid_map[pid] == TS_MAX_PIDS)
        return;

    if(mod->map)
    {
        const uint16_t custom_pid = mod->pid_map[pid];
        if(custom_pid)
        {
            memcpy(mod->custom_ts, ts, TS_PACKET_SIZE);
            TS_SET_PID(mod->custom_ts, custom_pid);
            module_stream_send(mod, mod->custom_ts);
            return;
        }
    }

    module_stream_send(mod, ts);
}

/*
 * oooo     oooo  ooooooo  ooooooooo  ooooo  oooo ooooo       ooooooooooo
 *  8888o   888 o888   888o 888    88o 888    88   888         888    88
 *  88 888o8 88 888     888 888    888 888    88   888         888ooo8
 *  88  888  88 888o   o888 888    888 888    88   888      o  888    oo
 * o88o  8  o88o  88ooo88  o888ooo88    888oo88   o888ooooo88 o888ooo8888
 *
 */

static void module_init(lua_State *L, module_data_t *mod)
{
    module_stream_init(L, mod, on_ts);
    module_demux_set(mod, NULL, NULL);

    module_option_string(L, "name", &mod->config.name, NULL);
    if(mod->config.name == NULL)
        luaL_error(L, "[channel] option 'name' is required");

    if(module_option_integer(L, "pnr", &mod->config.pnr))
    {
        module_option_integer(L, "set_pnr", &mod->config.set_pnr);

        module_option_boolean(L, "cas", &mod->config.cas);

        mod->pat = ts_psi_init(TS_TYPE_PAT, 0);
        mod->pmt = ts_psi_init(TS_TYPE_PMT, TS_MAX_PIDS);
        mod->custom_pat = ts_psi_init(TS_TYPE_PAT, 0);
        mod->custom_pmt = ts_psi_init(TS_TYPE_PMT, TS_MAX_PIDS);
        mod->stream[0] = TS_TYPE_PAT;
        module_demux_join(mod, 0);
        if(mod->config.cas)
        {
            mod->cat = ts_psi_init(TS_TYPE_CAT, 1);
            mod->custom_cat = ts_psi_init(TS_TYPE_CAT, 1);
            mod->stream[1] = TS_TYPE_CAT;
            module_demux_join(mod, 1);
        }

        module_option_boolean(L, "no_sdt", &mod->config.no_sdt);
        if(mod->config.no_sdt == false)
        {
            mod->sdt = ts_psi_init(TS_TYPE_SDT, 0x11);
            mod->custom_sdt = ts_psi_init(TS_TYPE_SDT, 0x11);
            mod->stream[0x11] = TS_TYPE_SDT;
            module_demux_join(mod, 0x11);

            module_option_boolean(L, "pass_sdt", &mod->config.pass_sdt);
        }

        module_option_boolean(L, "no_eit", &mod->config.no_eit);
        if(mod->config.no_eit == false)
        {
            mod->eit = ts_psi_init(TS_TYPE_EIT, 0x12);
            mod->stream[0x12] = TS_TYPE_EIT;
            module_demux_join(mod, 0x12);

            mod->stream[0x14] = TS_TYPE_TDT;
            module_demux_join(mod, 0x14);

            module_option_boolean(L, "pass_eit", &mod->config.pass_eit);
        }

        module_option_boolean(L, "no_reload", &mod->config.no_reload);
        if(mod->config.no_reload)
            mod->si_timer = asc_timer_init(500, on_si_timer, mod);
    }
    else
    {
        lua_getfield(L, MODULE_OPTIONS_IDX, "pid");
        if(lua_istable(L, -1))
        {
            lua_foreach(L, -2)
            {
                const int pid = lua_tointeger(L, -1);
                if(!ts_pid_valid(pid))
                    luaL_error(L, MSG("option 'pid': pid is out of range"));

                mod->stream[pid] = TS_TYPE_PES;
                module_demux_join(mod, pid);
            }
        }
        lua_pop(L, 1); // pid
    }

    lua_getfield(L, MODULE_OPTIONS_IDX, "map");
    if(lua_istable(L, -1))
    {
        mod->map = asc_list_init();
        lua_foreach(L, -2)
        {
            if(lua_type(L, -1) != LUA_TTABLE)
                luaL_error(L, MSG("option 'map': wrong type"));
            if(luaL_len(L, -1) != 2)
                luaL_error(L, MSG("option 'map': wrong format"));

            lua_rawgeti(L, -1, 1);
            const char *key = lua_tostring(L, -1);
            if(luaL_len(L, -1) > 5)
                luaL_error(L, MSG("option 'map': key is too large"));
            lua_pop(L, 1);

            lua_rawgeti(L, -1, 2);
            int val = lua_tointeger(L, -1);
            if(!(val > 0 && val < TS_NULL_PID))
                luaL_error(L, MSG("option 'map': value is out of range"));
            lua_pop(L, 1);

            map_item_t *const map_item = ASC_ALLOC(1, map_item_t);
            snprintf(map_item->type, sizeof(map_item->type), "%s", key);

            if(key[0] >= '1' && key[0] <= '9')
                map_item->origin_pid = atoi(key);
            map_item->custom_pid = val;
            asc_list_insert_tail(mod->map, map_item);
        }
    }
    lua_pop(L, 1); // map

    lua_getfield(L, MODULE_OPTIONS_IDX, "filter");
    if(lua_istable(L, -1))
    {
        lua_foreach(L, -2)
        {
            const int pid = lua_tointeger(L, -1);
            if(!ts_pid_valid(pid))
                luaL_error(L, MSG("option 'filter': pid is out of range"));

            mod->pid_map[pid] = TS_MAX_PIDS;
        }
    }
    lua_pop(L, 1); // filter

    lua_getfield(L, MODULE_OPTIONS_IDX, "filter~");
    if(lua_istable(L, -1))
    {
        for(uint32_t i = 0; i < ASC_ARRAY_SIZE(mod->pid_map); ++i)
            mod->pid_map[i] = TS_MAX_PIDS;

        lua_foreach(L, -2)
        {
            const int pid = lua_tointeger(L, -1);
            if(!ts_pid_valid(pid))
                luaL_error(L, MSG("option 'filter~': pid is out of range"));

            mod->pid_map[pid] = 0;
        }
    }
    lua_pop(L, 1); // filter~
}

static void module_destroy(module_data_t *mod)
{
    module_stream_destroy(mod);

    ts_psi_destroy(mod->pat);
    if(mod->cat)
    {
        ts_psi_destroy(mod->cat);
        ts_psi_destroy(mod->custom_cat);
    }
    ts_psi_destroy(mod->pmt);
    ts_psi_destroy(mod->custom_pat);
    ts_psi_destroy(mod->custom_pmt);

    if(mod->sdt)
    {
        ts_psi_destroy(mod->sdt);
        ts_psi_destroy(mod->custom_sdt);

        free(mod->sdt_checksum_list);
    }

    if(mod->eit)
        ts_psi_destroy(mod->eit);

    if(mod->map)
    {
        asc_list_for(mod->map)
        {
            free(asc_list_data(mod->map));
        }
        asc_list_destroy(mod->map);
    }

    ASC_FREE(mod->si_timer, asc_timer_destroy);
}

STREAM_MODULE_REGISTER(channel)
{
    .init = module_init,
    .destroy = module_destroy,
};
