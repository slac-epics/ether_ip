/* $Id$
 *
 * drvEtherIP
 *
 * vxWorks driver that uses ether_ip routines,
 * keeping lists of PLCs and tags and scanlists etc.
 *
 * kasemir@lanl.gov
 */

#include "drvEtherIP.h"
#include "stdio.h"

double drvEtherIP_default_rate = 0.0;

/* THE structure for this driver
 * Note that each PLC entry has it's own lock
 * for the scanlists & statistics.
 * Each PLC's scan task uses that per-PLC lock,
 * calls to loop/add/list PLCs also use this
 * more global lock.
 */
static struct
{
    DL_List /*PLC*/ PLCs;
    SEM_ID          lock; /* lock for PLCs list */
}   drvEtherIP_private;

/* Locking:
 *
 * Issues:
 * a) Structures: adding PLCs, scanlists, tags
 *                moving tags between scanlists,
 *                modifying callbacks for tags
 * b) Data:       driver and device read/write tag's data
 *                and change the update flag
 *
 * Locks:
 *
 * 1) drvEtherIP_private.lock is for PLCs
 *    Everything that accesses >1 PLC takes this lock
 *
 * 2) PLC.lock is per-PLC
 *    All structural changes to a PLC take this lock
 *    Currently PLCs are added, never removed,
 *    so the global lock is not affected by this
 *    PLC.lock is for all data structures for this PLC:
 *    scanlists, tags, callbacks.
 *
 *    PLC_scan_task needs access to connection & scanlists,
 *    so it takes lock for each run down the scanlist.
 *    (actually might update lame_count when not locked,
 *     but not fatal since PLC is never deleted)
 *
 * 3) TagInfo.data_lock is the Data lock.
 *    The scan task runs over the Tags in a scanlist three times:
 *    a) see how much can be handled in one network transfer,
 *       determine size of request/response
 *    b) setup the requests
 *    c) handle the response
 *
 *    The list of tags cannot change because of the PLC.lock.
 *    But the device might want to switch from read to write.
 *    In the protocol, the "CIP Read Data" and "CIP Write Data"
 *    request/response are different in length.
 *    So the do_write flag is checked in a), the driver has to
 *    know in b and c) if this is a write access.
 *    -> write behaviour must not change accross a->c.
 *    The network transfer between b) and c) takes time,
 *    so we avoid locking the data and do_write flag all that time
 *    from a) to c). Instead, the lock is released after b) and
 *    taken again in c).
 *    Data is locked in c) to keep the device from looking at
 *    immature data. The driver keeps the state of do_write from a)
 *    in is_writing. If the device sets do_write after a), it's
 *    ignored until the next scan. (Otherwise the device would
 *    be locked until the next scan!)
 *
 * do_write   is_writing
 *    1           0       -> Device support requested write
 *    1           1       -> Driver noticed the write request,
 *    0           1       -> sends it
 *    0           0       -> Driver received write result from PLC
 */

/* ------------------------------------------------------------
 * TagInfo
 * ------------------------------------------------------------ */

static void dump_TagInfo(const TagInfo *info, int level)
{
    char buffer[EIP_MAX_TAG_LENGTH];
    bool have_sem;
    
    printf("*** Tag '%s' @ 0x%X:\n", info->string_tag, (unsigned int)info);
    if (level > 3)
    {
        printf("  scanlist            : 0x%X\n",
               (unsigned int)info->scanlist);
        EIP_copy_ParsedTag(buffer, info->tag);
        printf("  compiled tag        : '%s'\n", buffer);
        printf("  elements            : %d\n", info->elements);
        printf("  cip_r_request_size  : %d\n", info->cip_r_request_size);
        printf("  cip_r_response_size : %d\n", info->cip_r_response_size);
        printf("  cip_w_request_size  : %d\n", info->cip_w_request_size);
        printf("  cip_w_response_size : %d\n", info->cip_w_response_size);
        printf("  data_lock ID        : 0x%X\n",
               (unsigned int) info->data_lock);
    }
    have_sem = semTake(info->data_lock, EIP_SEM_TIMEOUT) == OK;
    if (! have_sem)
        printf("  (CANNOT GET DATA LOCK!)\n");
    if (level > 3)
    {
        printf("  data_size (buffer)  : %d\n",  info->data_size);
        printf("  valid_data_size     : %d\n",  info->valid_data_size);
        printf("  do_write            : %s\n",
               (info->do_write ? "yes" : "no"));
        printf("  is_writing          : %s\n",
               (info->is_writing ? "yes" : "no"));
        printf("  data              : ");
    }
    if (info->valid_data_size > 0)
        dump_raw_CIP_data(info->data, info->elements);
    else
        printf("-no data-\n");
    if (have_sem)
        semGive(info->data_lock);
    if (level > 3)
        printf("  transfer tick-time  : %d (%g secs)\n",
               info->transfer_ticktime,
               (double)info->transfer_ticktime/sysClkRateGet());
}

static TagInfo *new_TagInfo(const char *string_tag, size_t elements)
{
    TagInfo *info = (TagInfo *) calloc(sizeof(TagInfo), 1);
    if (!info ||
        !EIP_strdup(&info->string_tag,
                    string_tag, strlen(string_tag)))
        return 0;
    info->tag = EIP_parse_tag(string_tag);
    if (! info->tag)
    {
        EIP_printf(2, "new_TagInfo: failed to parse tag '%s'\n",
                   string_tag);
        return 0;
    }
    info->elements = elements;
    info->data_lock = semMCreate(EIP_SEMOPTS);
    if (! info->data_lock)
    {
        EIP_printf(0, "new_TagInfo (%s): Cannot create semaphore\n",
                   string_tag);
        return 0;
    }
    DLL_init(&info->callbacks);
    return info;
}

#if 0
/* We never remove a tag */
static void free_TagInfo(TagInfo *info)
{
    EIP_free_ParsedTag(info->tag);
    free(info->string_tag);
    if (info->data_size > 0)
        free(info->data);
    semDelete(info->data_lock);
    free (info);
}
#endif

/* ------------------------------------------------------------
 * ScanList
 * ------------------------------------------------------------
 *
 * NOTE: None of these ScanList funcs do any locking,
 * The caller has to do that!
 */

static void dump_ScanList(const ScanList *list, int level)
{
    const TagInfo *info;

    printf("Scanlist          %g secs (%d ticks) @ 0x%X:\n",
           list->period, list->period_ticks, (unsigned int)list);
    printf("  Status        : %s\n",
           (list->enabled ? "enabled" : "DISABLED"));
    printf("  Last scan     : %ld ticks\n", list->scan_ticktime);
    if (level > 4)
    {
        printf("  Errors        : %u\n", list->list_errors);
        printf("  Next scan     : %ld ticks\n", list->scheduled_ticktime);
        printf("  Min. scan time: %d ticks (%g secs)\n",
               list->min_scan_ticks,
               (double)list->min_scan_ticks/sysClkRateGet());
        printf("  Max. scan time: %d ticks (%g secs)\n",
               list->max_scan_ticks,
               (double)list->max_scan_ticks/sysClkRateGet());
        printf("  Last scan time: %d ticks (%g secs)\n",
               list->last_scan_ticks,
               (double)list->last_scan_ticks/sysClkRateGet());
    }
    if (level > 5)
    {
        for (info=DLL_first(TagInfo, &list->taginfos); info;
             info=DLL_next(TagInfo, info))
            dump_TagInfo(info, level);
    }
}

/* Set counters etc. to initial values */
static void reset_ScanList(ScanList *scanlist)
{
    scanlist->enabled = true;
    scanlist->period_ticks = scanlist->period * sysClkRateGet();
    scanlist->list_errors = 0;
    scanlist->scheduled_ticktime = 0;
    scanlist->min_scan_ticks = ~((size_t)0);
    scanlist->max_scan_ticks = 0;
    scanlist->last_scan_ticks = 0;
}

static ScanList *new_ScanList(PLC *plc, double period)
{
    ScanList *list = (ScanList *) calloc(sizeof(ScanList), 1);
    if (!list)
        return 0;
    DLL_init(&list->taginfos);
    list->plc = plc;
    list->period = period;
    reset_ScanList (list);
    return list;
}

#if 0
/* We never remove a scan list */
static void free_ScanList(ScanList *scanlist)
{
    TagInfo *info;
    while ((info = DLL_decap(&scanlist->taginfos)) != 0)
        free_TagInfo(info);
    free(scanlist);
}
#endif

/* Find tag by name, returns 0 for "not found" */
static TagInfo *find_ScanList_Tag (const ScanList *scanlist,
                                   const char *string_tag)
{
    TagInfo *info;
    for (info=DLL_first(TagInfo, &scanlist->taginfos); info;
         info=DLL_next(TagInfo, info))
    {
        if (strcmp(info->string_tag, string_tag)==0)
            return info;
    }
    return 0;
}

/* remove/add TagInfo */
static void remove_ScanList_TagInfo(ScanList *scanlist, TagInfo *info)
{
    info->scanlist = 0;
    DLL_unlink(&scanlist->taginfos, info);
}

static void add_ScanList_TagInfo(ScanList *scanlist, TagInfo *info)
{
    DLL_append(&scanlist->taginfos, info);
    info->scanlist = scanlist;
}

/* Add new tag to taglist, compile tag
 * returns 0 on error */
static TagInfo *add_ScanList_Tag(ScanList *scanlist,
                                 const char *string_tag,
                                 size_t elements)
{
    TagInfo *info = new_TagInfo(string_tag, elements);
    if (info)
        add_ScanList_TagInfo(scanlist, info);
    return info;
}

/* ------------------------------------------------------------
 * PLC
 * ------------------------------------------------------------ */

static PLC *new_PLC(const char *name)
{
    PLC *plc = (PLC *) calloc (sizeof (PLC), 1);
    if (! (plc && EIP_strdup (&plc->name, name, strlen(name))))
        return 0;
    DLL_init (&plc->scanlists);
    plc->lock = semMCreate (EIP_SEMOPTS);
    if (! plc->lock)
    {
        EIP_printf (0, "new_PLC (%s): Cannot create semaphore\n", name);
        return 0;
    }
    return plc;
}

#if 0
/* We never really remove a PLC from the list,
 * but this is how it could be done. Maybe. */
static void free_PLC(PLC *plc)
{
    ScanList *list;
    
    semDelete(plc->lock);
    free(plc->name);
    free(plc->ip_addr);
    while ((list = DLL_decap(&plc->scanlists)) != 0)
        free_ScanList(list);
    free(plc);
}
#endif

/* After TagInfos are defined (tag & elements are set),
 * fill rest of TagInfo: request/response size.
 *
 * Returns OK if any TagInfo in the scanlists could be filled,
 * so we believe that scanning this PLC makes some sense.
 */
static bool complete_PLC_ScanList_TagInfos(PLC *plc)
{
    ScanList       *list;
    TagInfo        *info;
    const CN_USINT *data;
    bool           any_ok = false;
    size_t         type_and_data_len;

    for (list=DLL_first(ScanList, &plc->scanlists); list;
         list=DLL_next(ScanList, list))
    {
        for (info=DLL_first(TagInfo, &list->taginfos); info;
             info=DLL_next(TagInfo, info))
        {
            if (info->cip_r_request_size || info->cip_r_response_size)
                continue;           /* don't look twice */
            data = EIP_read_tag(&plc->connection,
                                info->tag, info->elements,
                                NULL /* data_size */,
                                &info->cip_r_request_size,
                                &info->cip_r_response_size);
            if (data)
            {
                any_ok = true;
                /* Estimate write sizes from the request/response for read */
                if (info->cip_r_response_size <= 4)
                {
                    info->cip_w_request_size  = 0;
                    info->cip_w_response_size = 0;
                }
                else
                {
                    type_and_data_len = info->cip_r_response_size - 4;
                    info->cip_w_request_size  = info->cip_r_request_size
                                                + type_and_data_len;
                    info->cip_w_response_size = 4;
                }
            }
            else
            {
                info->cip_r_request_size  = 0;
                info->cip_r_response_size = 0;
                info->cip_w_request_size  = 0;
                info->cip_w_response_size = 0;
            }
        }
    }
    return any_ok;
}

static void invalidate_PLC_tags(PLC *plc)
{
    ScanList *list;
    TagInfo  *info;
    
    for (list=DLL_first(ScanList,&plc->scanlists);
         list;
         list=DLL_next(ScanList,list))
    {
        for (info = DLL_first(TagInfo, &list->taginfos);
             info;
             info = DLL_next(TagInfo, info))
        {
            if (semTake(info->data_lock, EIP_SEM_TIMEOUT) == OK)
            {
                info->valid_data_size = 0;
                semGive(info->data_lock);
            }
        }
    }
}

/* Test if we are connected, if not try to connect to PLC */
static bool assert_PLC_connect(PLC *plc)
{
    if (plc->connection.sock)
        return true;
    return EIP_startup(&plc->connection, plc->ip_addr,
                       ETHERIP_PORT, plc->slot, ETHERIP_TIMEOUT)
        && complete_PLC_ScanList_TagInfos(plc);
}

static void disconnect_PLC(PLC *plc)
{
    if (plc->connection.sock)
    {
        EIP_shutdown(&plc->connection);
        invalidate_PLC_tags(plc);
    }
}

/* Given a transfer buffer limit,
 * see how many requests/responses can be handled in one transfer,
 * starting with the current TagInfo and using the following ones.
 *
 * Returns count,
 * fills sizes for total requests/responses as well as
 * size of MultiRequest/Response.
 *
 * Called by scan task, PLC is locked.
 */
static size_t determine_MultiRequest_count(size_t limit,
                                           TagInfo *info,
                                           size_t *requests_size,
                                           size_t *responses_size,
                                           size_t *multi_request_size,
                                           size_t *multi_response_size)
{
    size_t try_req, try_resp, count;
    /* Sum sizes for requests and responses,
     * determine total for MultiRequest/Response,
     * stop if too big.
     * Skip entries with empty cip_*_request_size!
     */
    count = *requests_size = *responses_size = 0;
    for (; info; info = DLL_next(TagInfo, info))
    {
        if (info->cip_r_request_size <= 0)
            continue;
        if (semTake(info->data_lock, EIP_SEM_TIMEOUT) != OK)
        {
            EIP_printf(1, "EIP determine_MultiRequest_count cannot lock %s\n",
                       info->string_tag);
            return 0;
        }
        if (info->do_write)
        {
            info->is_writing = true;
            try_req  = *requests_size  + info->cip_w_request_size;
            try_resp = *responses_size + info->cip_w_response_size;
        }
        else
        {
            try_req  = *requests_size  + info->cip_r_request_size;
            try_resp = *responses_size + info->cip_r_response_size;
        }
        semGive(info->data_lock);
        *multi_request_size  = CIP_MultiRequest_size (count+1, try_req);
        *multi_response_size = CIP_MultiResponse_size(count+1, try_resp);
        if (*multi_request_size  > limit ||
            *multi_response_size > limit)
        {   /* more won't fit */
            *multi_request_size =CIP_MultiRequest_size (count,*requests_size);
            *multi_response_size=CIP_MultiResponse_size(count,*responses_size);
            return count;
        }
        ++count; /* ok, include another request */
        *requests_size  = try_req;
        *responses_size = try_resp;
    }
    return count;
}

/* Read all tags in Scanlist,
 * using MultiRequests for as many as possible.
 * Called by scan task, PLC is locked.
 *
 * Returns OK when the transactions worked out,
 * even if the read requests for the tags
 * returned no data.
 */
static bool process_ScanList(EIPConnection *c, ScanList *scanlist)
{
    TagInfo             *info, *info_position;
    size_t              count, requests_size, responses_size;
    size_t              multi_request_size, multi_response_size;
    size_t              send_size, i, elements;
    CN_USINT            *send_request, *multi_request, *request;
    const CN_USINT      *response, *single_response, *data;
    EncapsulationRRData rr_data;
    size_t              single_response_size, data_size, transfer_ticktime;
    TagCallback         *cb;
    bool                ok;

    info = DLL_first(TagInfo, &scanlist->taginfos);
    while (info)
    {   /* keep position, we'll loop several times:
         * 0) in determine_MultiRequest_count
         * 1) to send out the requests
         * 2) to handle the responses
         */
        info_position = info;
        count = determine_MultiRequest_count(c->transfer_buffer_limit,
                                             info,
                                             &requests_size,
                                             &responses_size,
                                             &multi_request_size,
                                             &multi_response_size);
        if (count == 0)
            return true;
        /* send <count> requests as one transfer */
        send_size = CM_Unconnected_Send_size(multi_request_size);
        EIP_printf(10, " ------------------- New Request ------------\n");
        send_request = EIP_make_SendRRData(c, send_size);
        if (!send_request)
            return false;
        multi_request = make_CM_Unconnected_Send(send_request,
                                                 multi_request_size, c->slot);
        if (!multi_request  ||
            !prepare_CIP_MultiRequest(multi_request, count))
            return false;
        /* Add read/write requests to the multi requests */
        for (i=0; i<count; info=DLL_next(TagInfo, info))
        {
            if (info->cip_r_request_size <= 0)
                continue;
            EIP_printf(10, "Request #%d (%s):\n", i, info->string_tag);
            if (info->is_writing)
            {
                request = CIP_MultiRequest_item(multi_request,
                                                i, info->cip_w_request_size);
                if (semTake(info->data_lock, EIP_SEM_TIMEOUT) != OK)
                {
                    EIP_printf(1, "EIP process_ScanList '%s': "
                               "no data lock (write)\n", info->string_tag);
                    return false;
                }
                ok = request &&
                    make_CIP_WriteData(
                        request, info->tag,
                        (CIP_Type)get_CIP_typecode(info->data),
                        info->elements, info->data + CIP_Typecode_size);
                info->do_write = false;
                semGive(info->data_lock);
            }
            else
            {   /* reading, !is_writing */
                request = CIP_MultiRequest_item(
                    multi_request, i, info->cip_r_request_size);
                ok = request &&
                    make_CIP_ReadData(request, info->tag, info->elements);
            }
            if (!ok)
                return false;
            ++i; /* increment here, not in for() -> skip empty tags */
        } /* for i=0..count */
        transfer_ticktime = tickGet();
        if (!EIP_send_connection_buffer(c))
            return false;
        /* read & disassemble response */
        if (!EIP_read_connection_buffer(c))
        {
            EIP_printf(2, "EIP process_ScanList: No response\n");
            return false;
        }
        transfer_ticktime = tickGet() - transfer_ticktime;
        response = EIP_unpack_RRData(c->buffer, &rr_data);
        if (! check_CIP_MultiRequest_Response(response,
                                              rr_data.data_length))
        {
            EIP_printf(2, "EIP process_ScanList: Error in response\n");
            for (info=info_position,i=0; i<count; info=DLL_next(TagInfo, info))
            {
                if (info->cip_r_request_size <= 0)
                    continue;
                EIP_printf(2, "Tag %i: '%s'\n", i, info->string_tag);
                ++i;
            }
            if (EIP_verbosity >= 2)
                dump_CIP_MultiRequest_Response_Error(response,
                                                     rr_data.data_length);
            return false;
        }
        /* Handle individual read/write responses */
        for (info=info_position, i=0; i<count; info=DLL_next(TagInfo, info))
        {
            if (info->cip_r_request_size <= 0)
                continue;
            info->transfer_ticktime = transfer_ticktime;
            single_response = get_CIP_MultiRequest_Response(
                response, rr_data.data_length, i, &single_response_size);
            if (! single_response)
                return false;
            if (EIP_verbosity >= 10)
            {
                EIP_printf(10, "Response #%d (%s):\n", i, info->string_tag);
                EIP_dump_raw_MR_Response(single_response, 0);
            }
            if (semTake(info->data_lock, EIP_SEM_TIMEOUT) != OK)
            {
                EIP_printf(1, "EIP process_ScanList '%s': "
                           "no data lock (receive)\n", info->string_tag);
                return false;
            }
            if (info->is_writing)
            {
                if (!check_CIP_WriteData_Response(single_response,
                                                  single_response_size))
                {
                    EIP_printf(0, "EIP: CIPWrite failed for '%s'\n",
                               info->string_tag);
                    info->valid_data_size = 0;
                }
                info->is_writing = false;
            }
            else /* not writing, reading */
            {
                data = check_CIP_ReadData_Response(
                    single_response, single_response_size, &data_size);
                if (info->do_write)
                {   /* Possible: Read request ... network delay ... response.
                     * Ignore the read, next scan will write */
                    EIP_printf(8, "EIP '%s': Device support requested write "
                               "in middle of read cycle.\n", info->string_tag);
                }
                else
                {
                    if (data_size > 0  &&
                        EIP_reserve_buffer((void **)&info->data,
                                           &info->data_size, data_size))
                    {
                        memcpy(info->data, data, data_size);
                        info->valid_data_size = data_size;
                        if (EIP_verbosity >= 10)
                        {
                            elements = CIP_Type_size(get_CIP_typecode(data));
                            if (elements > 0)
                            {
                                elements = data_size / elements;
                                dump_raw_CIP_data(data, elements);
                            }
                            else
                                EIP_printf(10, "Unknown Data type:\n");
                        }
                    }
                    else
                        info->valid_data_size = 0;
                }
            }
            /* Call all registered callbacks for this tag */
            for (cb = DLL_first(TagCallback,&info->callbacks);
                 cb; cb=DLL_next(TagCallback,cb))
                (*cb->callback) (cb->arg);
            semGive(info->data_lock);
            ++i;
        }
        /* "info" now on next unread TagInfo or 0 */
    } /* while "info" ... */
    return true;
}

/* Scan task, one per PLC */
static void PLC_scan_task(PLC *plc)
{
    ScanList *list;
    ULONG    next_schedule_ticks, start_ticks;
    int      timeout_ticks;
    bool     transfer_ok;

    timeout_ticks = sysClkRateGet() * plc->connection.millisec_timeout / 1000;
    if (timeout_ticks <= 100)
        timeout_ticks = 100;

  scan_loop:
    next_schedule_ticks = 0;
    if (semTake(plc->lock, WAIT_FOREVER) == ERROR)
    {
        EIP_printf(1, "drvEtherIP scan task for PLC '%s'"
                   " cannot take plc->lock\n", plc->name);
        return;
    }
    if (!assert_PLC_connect(plc))
    {   /* don't rush since connection takes network bandwidth */
        semGive(plc->lock);
        taskDelay(timeout_ticks);
        goto scan_loop;
    }
    for (list=DLL_first(ScanList,&plc->scanlists);
         list;
         list=DLL_next(ScanList,list))
    {
        if (! list->enabled)
            continue;
        start_ticks = tickGet();
        if (start_ticks >= list->scheduled_ticktime)
        {
            list->scan_ticktime = start_ticks;
            transfer_ok = process_ScanList(&plc->connection, list);
            list->last_scan_ticks = tickGet() - start_ticks;
            /* update statistics */
            if (list->last_scan_ticks > list->max_scan_ticks)
                list->max_scan_ticks = list->last_scan_ticks;
            if (list->last_scan_ticks < list->min_scan_ticks)
                list->min_scan_ticks = list->last_scan_ticks;
            if (transfer_ok) /* reschedule exactly */
                list->scheduled_ticktime = start_ticks+list->period_ticks;
            else
            {   /* delay: ignore extra due to error/timeout */
                ++list->list_errors;
                ++plc->plc_errors;
                list->scheduled_ticktime = tickGet() + timeout_ticks;
                disconnect_PLC(plc);
                semGive (plc->lock);
                goto scan_loop;
            }
        }
        /* Update tick time for closest list */
        if (next_schedule_ticks <= 0 ||
            next_schedule_ticks > list->scheduled_ticktime)
            next_schedule_ticks = list->scheduled_ticktime;
    }
    semGive(plc->lock);
    /* sleep until next turn */
    if (next_schedule_ticks > 0)
    {
        start_ticks = tickGet();
        if (start_ticks < next_schedule_ticks)
            taskDelay(next_schedule_ticks - tickGet());
        else /* no time to spare, getting behind: */
            ++plc->slow_scans; /* hmm, "plc" not locked... */
    }
    else
        taskDelay(10); /* fallback for empty/degenerate scan list */
    goto scan_loop;
}

/* Find PLC entry by name, maybe create a new one if not found */
static PLC *get_PLC(const char *name, bool create)
{
    PLC *plc;
    for (plc = DLL_first(PLC,&drvEtherIP_private.PLCs);
         plc; plc=DLL_next(PLC,plc))
    {
        if (strcmp(plc->name, name) == 0)
            return plc;
    }
    if (! create)
        return 0;
    plc = new_PLC(name);
    if (plc)
        DLL_append(&drvEtherIP_private.PLCs, plc);
    return plc;
}

/* get (or create) ScanList for given rate */
static ScanList *get_PLC_ScanList(PLC *plc, double period, bool create)
{
    ScanList *list;
    for (list=DLL_first(ScanList, &plc->scanlists); list;
         list=DLL_next(ScanList, list))
    {
        if (list->period == period)
            return list;
    }
    if (! create)
        return 0;
    list = new_ScanList(plc, period);
    if (list)
        DLL_append(&plc->scanlists, list);
    
    return list;
}

/* Find ScanList and TagInfo for given tag.
 * On success, pointer to ScanList and TagInfo are filled.
 */
static bool find_PLC_tag(PLC *plc,
                         const char *string_tag,
                         ScanList **list,
                         TagInfo **info)
{
     for (*list=DLL_first(ScanList,&plc->scanlists);
         *list;
         *list=DLL_next(ScanList,*list))
    {
        *info = find_ScanList_Tag(*list, string_tag);
        if (*info)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------
 * public interface
 * ------------------------------------------------------------ */

void drvEtherIP_init ()
{
    if (drvEtherIP_private.lock)
    {
        EIP_printf (0, "drvEtherIP_init called more than once!\n");
        return;
    }
    drvEtherIP_private.lock = semMCreate (EIP_SEMOPTS);
    if (! drvEtherIP_private.lock)
        EIP_printf (0, "drvEtherIP_init cannot create semaphore!\n");
    DLL_init (&drvEtherIP_private.PLCs);
}

void drvEtherIP_help()
{
    printf("drvEtherIP V%d.%d diagnostics routines:\n",
           ETHERIP_MAYOR, ETHERIP_MINOR);
    printf("    int EIP_verbosity:\n");
    printf("    -  set to 0..10\n");
    printf("    double drvEtherIP_default_rate = <seconds>\n");
    printf("    -  define the default scan rate\n");
    printf("       (if neither SCAN nor INP/OUT provide one)\n");
    printf("    drvEtherIP_define_PLC <name>, <ip_addr>, <slot>\n");
    printf("    -  define a PLC name (used by EPICS records) as IP\n");
    printf("       (DNS name or dot-notation) and slot (0...)\n");
    printf("    drvEtherIP_read_tag <ip>, <slot>, <tag>, <elm.>, <timeout>\n");
    printf("    -  call to test a round-trip single tag read\n");
    printf("       ip: IP address (numbers or name known by vxWorks\n");
    printf("       slot: Slot of the PLC controller (not ENET). 0, 1, ...\n");
    printf("       timeout: milliseconds\n");
    printf("    drvEtherIP_report <level>\n");
    printf("    -  level = 0..10\n");
    printf("    drvEtherIP_dump\n");
    printf("    -  dump all tags and values; short version of drvEtherIP_report\n");
    printf("    drvEtherIP_reset_statistics\n");
    printf("    -  reset error counts and min/max scan times\n");
    printf("    drvEtherIP_restart\n");
    printf("    -  in case of communication errors, driver will restart,\n");
    printf("       so calling this one directly shouldn't be necessary\n");
    printf("       but is possible\n");
    printf("\n");
}

long drvEtherIP_report(int level)
{
    PLC *plc;
    EIPIdentityInfo *ident;
    ScanList *list;
    bool have_drvlock, have_PLClock;
    
    printf("drvEtherIP V%d.%d report, -*- outline -*-\n",
           ETHERIP_MAYOR, ETHERIP_MINOR);
    if (level > 0)
        printf("  SEM_ID lock: 0x%X\n",
               (unsigned int) drvEtherIP_private.lock);
    have_drvlock = semTake(drvEtherIP_private.lock, EIP_SEM_TIMEOUT*5) == OK;
    if (! have_drvlock)
        printf("   CANNOT GET DRIVER'S LOCK!\n");
    for (plc = DLL_first(PLC,&drvEtherIP_private.PLCs);
         plc; plc=DLL_next(PLC,plc))
    {
        printf ("* PLC '%s', IP '%s':\n", plc->name, plc->ip_addr);
        if (level > 0)
        {
            ident = &plc->connection.info;
            printf("  Interface name        : %s\n", ident->name);
            printf("  Interface vendor      : 0x%X\n", ident->vendor);
            printf("  Interface type        : 0x%X\n", ident->device_type);
            printf("  Interface revision    : 0x%X\n", ident->revision);
            printf("  Interface serial      : 0x%X\n",
                   (unsigned)ident->serial_number);
            
            printf("  scan thread slow count: %u\n", plc->slow_scans);
            printf("  connection errors     : %u\n", plc->plc_errors);
        }
        if (level > 1)
        {
            printf("  SEM_ID lock           : 0x%X\n",
                   (unsigned int) plc->lock);
            printf("  scan task ID          : 0x%X (%s)\n",
                   plc->scan_task_id,
                   (taskIdVerify(plc->scan_task_id) == OK ?
                    "running" : "-dead-"));
            have_PLClock = semTake(plc->lock, EIP_SEM_TIMEOUT*5) == OK;
            if (! have_PLClock)
                printf("   CANNOT GET PLC'S LOCK!\n");
            if (level > 2)
            {
                printf("** ");
                EIP_dump_connection(&plc->connection);
            }
            if (level > 3)
            {
                for (list=DLL_first(ScanList, &plc->scanlists); list;
                     list=DLL_next(ScanList, list))
                {
                    printf("** ");
                    dump_ScanList(list, level);
                }
            }
            if (have_PLClock)
                semGive(plc->lock);
        }
    }
    if (have_drvlock)
        semGive(drvEtherIP_private.lock);
    printf("\n");
    return 0;
}

void drvEtherIP_dump ()
{
    PLC      *plc;
    ScanList *list;
    TagInfo  *info;
    
    semTake(drvEtherIP_private.lock, WAIT_FOREVER);
    for (plc = DLL_first(PLC,&drvEtherIP_private.PLCs);
         plc; plc=DLL_next(PLC,plc))
    {
        semTake(plc->lock, WAIT_FOREVER);
        printf ("PLC %s\n", plc->name);
        for (list=DLL_first(ScanList, &plc->scanlists); list;
             list=DLL_next(ScanList, list))
        {
            for (info=DLL_first(TagInfo, &list->taginfos); info;
                 info=DLL_next(TagInfo, info))
            {
                printf ("%s ", info->string_tag);
                if (info->valid_data_size >= 0)
                    dump_raw_CIP_data (info->data, info->elements);
                else
                    printf (" - no data -\n");
            }
        }
        semGive (plc->lock);
    }
    semGive (drvEtherIP_private.lock);
    printf ("\n");
}

void drvEtherIP_reset_statistics ()
{
    PLC *plc;
    ScanList *list;

    semTake(drvEtherIP_private.lock, WAIT_FOREVER);
    for (plc = DLL_first(PLC,&drvEtherIP_private.PLCs);
         plc; plc=DLL_next(PLC,plc))
    {
        semTake(plc->lock, WAIT_FOREVER);
        plc->plc_errors = 0;
        plc->slow_scans = 0;
        for (list=DLL_first(ScanList, &plc->scanlists); list;
             list=DLL_next(ScanList, list))
            reset_ScanList (list);
        semGive (plc->lock);
    }
    semGive (drvEtherIP_private.lock);
}

/* Create a PLC entry:
 * name : identifier
 * ip_address: DNS name or dot-notation
 */
bool drvEtherIP_define_PLC(const char *PLC_name, const char *ip_addr, int slot)
{
    PLC *plc;

    semTake(drvEtherIP_private.lock, WAIT_FOREVER);
    plc = get_PLC(PLC_name, true);
    if (! plc)
    {
        semGive(drvEtherIP_private.lock);
        return false;
    }
    EIP_strdup(&plc->ip_addr, ip_addr, strlen(ip_addr));
    plc->slot = slot;
    semGive(drvEtherIP_private.lock);
    return true;
}

/* Returns PLC or 0 if not found */
PLC *drvEtherIP_find_PLC (const char *PLC_name)
{
    PLC *plc;

    semTake(drvEtherIP_private.lock, WAIT_FOREVER);
    plc = get_PLC (PLC_name, /*create*/ false);
    semGive (drvEtherIP_private.lock);

    return plc;
}

/* After the PLC is defined with drvEtherIP_define_PLC,
 * tags can be added
 */
TagInfo *drvEtherIP_add_tag(PLC *plc, double period,
                            const char *string_tag, size_t elements)
{
    ScanList *list;
    TagInfo  *info;

    semTake(plc->lock, WAIT_FOREVER);
    if (find_PLC_tag(plc, string_tag, &list, &info))
    {   /* check if period is OK */
        if (list->period > period)
        {   /* current scanlist is too slow */
            remove_ScanList_TagInfo(list, info);
            list = get_PLC_ScanList(plc, period, true);
            if (!list)
            {
                semGive(plc->lock);
                EIP_printf(2, "drvEtherIP: cannot create list at %g secs"
                           "for tag '%s'\n", period, string_tag);
                return 0;
            }
            add_ScanList_TagInfo(list, info);
        }
        if (info->elements < elements)  /* maximize element count */
            info->elements = elements;
    }
    else
    {   /* new tag */
        list = get_PLC_ScanList(plc, period, true);
        if (list)
            info = add_ScanList_Tag(list, string_tag, elements);
        else
        {
            EIP_printf(2, "drvEtherIP: cannot create list at %g secs"
                       "for tag '%s'\n", period, string_tag);
            info = 0;
        }
    }
    semGive(plc->lock);
    
    return info;
}

void  drvEtherIP_add_callback (PLC *plc, TagInfo *info,
                               EIPCallback callback, void *arg)
{
    TagCallback *cb;
    
    semTake(plc->lock, WAIT_FOREVER);
    for (cb = DLL_first(TagCallback, &info->callbacks);
         cb; cb = DLL_next(TagCallback, cb))
    {
        if (cb->callback == callback &&
            cb->arg      == arg)
        {
            semGive (plc->lock);
            return;
        }
    }
    /* Add new one */
    cb = (TagCallback *) malloc (sizeof (TagCallback));
    if (! cb)
        return;
    
    cb->callback = callback;
    cb->arg      = arg;
    DLL_append (&info->callbacks, cb);
    semGive (plc->lock);
}

void drvEtherIP_remove_callback (PLC *plc, TagInfo *info,
                                 EIPCallback callback, void *arg)
{
    TagCallback *cb;

    semTake(plc->lock, WAIT_FOREVER);
    for (cb = DLL_first(TagCallback, &info->callbacks);
         cb; cb=DLL_next(TagCallback, cb))
    {
        if (cb->callback == callback &&
            cb->arg      == arg)
        {
            DLL_unlink (&info->callbacks, cb);
            free (cb);
            break;
        }
    }
    semGive (plc->lock);
}

/* (Re-)connect to IOC,
 * (Re-)start scan tasks, one per PLC.
 * Returns number of tasks spawned.
 */
int drvEtherIP_restart()
{
    PLC    *plc;
    char   taskname[20];
    int    tasks = 0;
    size_t len;
    
    semTake(drvEtherIP_private.lock, WAIT_FOREVER);
    for (plc = DLL_first(PLC,&drvEtherIP_private.PLCs);
         plc; plc=DLL_next(PLC,plc))
    {
        /* block scan task (if running): */
        semTake(plc->lock, WAIT_FOREVER);
        /* restart the connection:
         * disconnect, PLC_scan_task will reconnect */
        disconnect_PLC(plc);
        /* check the scan task */
        if (plc->scan_task_id==0 || taskIdVerify(plc->scan_task_id)==ERROR)
        {
            len = strlen(plc->name);
            if (len > 16)
                len = 16;
            taskname[0] = 'E';
            taskname[1] = 'I';
            taskname[2] = 'P';
            memcpy(&taskname[3], plc->name, len);
            taskname[len+3] = '\0';
            plc->scan_task_id = taskSpawn(taskname,
                                          EIPSCAN_PRI,
                                          EIPSCAN_OPT,
                                          EIPSCAN_STACK,
                                          (FUNCPTR) PLC_scan_task,
                                          (int) plc,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0);
            EIP_printf(5, "drvEtherIP: launch scan task for PLC '%s'\n",
                       plc->name);
            ++tasks;
        }
        semGive(plc->lock);
    }
    semGive(drvEtherIP_private.lock);
    return tasks;
}
    
/* Command-line communication test,
 * not used by the driver */
int drvEtherIP_read_tag(const char *ip_addr,
                        int slot,
                        const char *tag_name,
                        int elements,
                        int timeout)
{
    EIPConnection  c;
    unsigned short port = ETHERIP_PORT;
    size_t         millisec_timeout = timeout;
    ParsedTag      *tag;
    const CN_USINT *data;
    size_t         data_size;
            
    if (! EIP_startup(&c, ip_addr, port, slot,
                      millisec_timeout))
        return -1;
    tag = EIP_parse_tag(tag_name);
    if (tag)
    {
        data = EIP_read_tag(&c, tag, elements, &data_size, 0, 0);
        if (data)
            dump_raw_CIP_data(data, elements);
        EIP_free_ParsedTag(tag);
    }
    EIP_shutdown(&c);

    return 0;
}

/* EPICS driver support entry table */
struct
{
    long number;
    long (* report) ();
    long (* inie) ();
} drvEtherIP =
{
    2,
    drvEtherIP_report,
    NULL
};

