/* $Id$
 *
 * EtherNet/IP: Ethernet - ControlNet
 *
 * kasemir@lanl.gov
 */

#include "ether_ip.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#ifndef vxWorks
#include <memory.h>
#endif

static const CN_UINT __endian_test = 0x0001;
#define is_little_endian (*((const CN_USINT*)&__endian_test))

/* Works for VxWorks' gcc, Linux and MS VC: */
#if 0
#define INLINE  __inline
#else
#define INLINE  /* */
#endif

/* Pack binary data in ControlNet format (little endian)
 *
 * Originally "pack" and unpack were modeled after the suggestions
 * in Kerningham/Pike's "Practice of Programming", taking a format argument
 * and a valiable list of values:
 *    s   - CN_SINT
 *    i   - CN_INT
 *    d   - CN_DINT
 *    r   - CN_REAL
 *
 * But that didn't work on vxWorks:
 * I couldn't pass CN_USINT for pack(), va_arg (ap, CN_USINT) always took
 * more than one byte from the stack.
 * The unpack seems to work because all var-args are pointer-sized.
 */

INLINE CN_USINT *pack_USINT (CN_USINT *buffer, CN_USINT val)
{
    *buffer++ = val;
    return buffer;
}

INLINE CN_USINT *pack_UINT (CN_USINT *buffer, CN_UINT val)
{
    *buffer++ =  val & 0x00FF;
    *buffer++ = (val & 0xFF00) >> 8;
    return buffer;
}

INLINE CN_USINT *pack_UDINT (CN_USINT *buffer, CN_UDINT val)
{
    *buffer++ =  val & 0x000000FF;
    *buffer++ = (val & 0x0000FF00) >> 8;
    *buffer++ = (val & 0x00FF0000) >> 16;
    *buffer++ = (val & 0xFF000000) >> 24;
    return buffer;
}

INLINE CN_USINT *pack_REAL (CN_USINT *buffer, CN_REAL val)
{
    const CN_USINT *p;
    
    if (is_little_endian)
    {
        p = (const CN_USINT *) &val;
        *buffer++ = *p++;
        *buffer++ = *p++;
        *buffer++ = *p++;
        *buffer++ = *p;
    }
    else
    {
        p = ((const CN_USINT *) &val)+3;
        *buffer++ = *p--;
        *buffer++ = *p--;
        *buffer++ = *p--;
        *buffer++ = *p;
    }
    return buffer;
}

INLINE const CN_USINT *unpack_UINT  (const CN_USINT *buffer, CN_UINT *val)
{
    *val =  buffer[0]
        | (buffer[1]<<8);
    return buffer + 2;
}

INLINE const CN_USINT *unpack_UDINT  (const CN_USINT *buffer, CN_UDINT *val)
{
    *val =  buffer[0]
        | (buffer[1]<< 8)
        | (buffer[2]<<16)
        | (buffer[3]<<24);
    return buffer + 4;
}

INLINE const CN_USINT *unpack_REAL  (const CN_USINT *buffer, CN_REAL *val)
{
    CN_USINT *p;
    if (is_little_endian)
    {
        p = (CN_USINT *) val;
        *p++ = *buffer++;
        *p++ = *buffer++;
        *p++ = *buffer++;
        *p   = *buffer++;
    }
    else
    {
        p = ((CN_USINT *) val)+3;
        *p-- = *buffer++;
        *p-- = *buffer++;
        *p-- = *buffer++;
        *p   = *buffer++;
    }
    return buffer + 4;
}

/* Format: like pack, but uppercase characters
 * are skipped in buffer, no result is assigned
 */
static const CN_USINT *unpack  (const CN_USINT *buffer,
                                const char *format, ...)
{
    va_list  ap;
    CN_USINT *vs;
    CN_UINT  *vi;
    CN_UDINT *vd;
    CN_REAL  *vr;

    va_start(ap, format);
    while (*format)
    {
        switch (*format)
        {
            case 's':
                vs = va_arg(ap, CN_USINT *);
                *vs = *buffer;
                /* drop */
            case 'S':
                ++buffer;
                break;
            case 'i':
                vi = va_arg(ap, CN_UINT *);
                buffer = unpack_UINT (buffer, vi);
                break;
            case 'I':
                buffer += 2;
                break;
            case 'd':
                vd = va_arg(ap, CN_UDINT *);
                buffer = unpack_UDINT (buffer, vd);
                break;
            case 'D':
                buffer += 4;
                break;
            case 'r':
                vr = va_arg(ap, CN_REAL *);
                buffer = unpack_REAL (buffer, vr);
                break;
            case 'R':
                buffer += 4;
                break;
            default:
                va_end(ap);
                return 0;
        }
        ++format;
    }
    va_end(ap);

    return buffer;
}

int EIP_verbosity = 10;

/* printf with EIP_verbosity check */
void EIP_printf (int level, const char *format, ...)
{
    va_list ap;
    if (level > EIP_verbosity)
        return;
    va_start(ap, format);
    vfprintf (stderr, format, ap);
    va_end(ap);              
}

void EIP_hexdump (const void *_data, int len)
{
    const char *data = _data;
    int offset = 0;
    int i;
#define NUM 16

    while (offset < len)
    {
        EIP_printf (0, "%08X ", offset);
        for (i=0; i<NUM; ++i)
            if ((i+offset)<len)
                EIP_printf (0, "%02X ", data[i] & 0xFF);
            else
                EIP_printf (0, "   ");

        EIP_printf (0, "- ");
        for (i=0; i<NUM  &&  (i+offset)<len; ++i)
            if (isprint(data[i]))
                EIP_printf (0, "%c", data[i]);
            else
                EIP_printf (0, ".");
        EIP_printf (0, "\n");

        offset += NUM;
        data += NUM;
    }
#undef NUM
}

/********************************************************
 * Message Router: Path
 ********************************************************/

static const char *EIP_class_name (CN_Classes c)
{
    switch (c)
    {
    case C_Identity:            return "Itentity";
    case C_MessageRouter:       return "MessageRouter";
    case C_ConnectionManager:   return "ConnectionManager";
    default:                    return "<unknown>";
    }
}

/* port path: currently supports only ports 0..14 ! */
static size_t port_path_size (CN_USINT port, CN_USINT link)
{
    return 1; /* this would change for >14 ! */
}

static void make_port_path (CN_USINT *path, CN_USINT port, CN_USINT link)
{
    path[0] = port;
    path[1] = link;
}

/* Build path from Class, Instance, Attribute (0 for no attr.) */
static size_t CIA_path_size (CN_Classes cls, CN_USINT instance, CN_USINT attr)
{   /* In Words */
    return attr ? 3 : 2;
}

/* Fill path buffer with path, return following location */
static CN_USINT *make_CIA_path (CN_USINT *path,
                                CN_Classes cls, CN_USINT instance,
                                CN_USINT attr)
{
    *path++ = 0x20;
    *path++ = cls;
    *path++ = 0x24;
    *path++ = instance;
    if (attr)
    {
        *path++ = 0x30;
        *path++ = attr;
    }
    return path;
}

/* A tad like the original strdup (not available for vxWorks),
 * but frees the original string if occupied
 * -> has to be 0-initialized */
bool EIP_strdup (char **ptr, const char *text, size_t len)
{
    if (*ptr)
        free (*ptr);
    *ptr = malloc (len+1);
    if (! *ptr)
    {
        EIP_printf (0, "no mem in EIP_strdup (%d bytes)\n", len);
        return false;
    }
    memcpy (*ptr, text, len);
    (*ptr)[len] = '\0';
    return true;
}

/* Append new node to ParsedTag */
static void append_tag (ParsedTag **tl, ParsedTag *node)
{
    while (*tl)
        tl = & (*tl)->next;
    *tl = node;
}

/* Turn tag string into ParsedTag-list */
ParsedTag *EIP_parse_tag (const char *tag)
{
    ParsedTag *tl = 0;  /* Tag list, initially empty */
    ParsedTag *node;
    size_t len;

    while (tag)
    {
        len = strcspn (tag, ".[");
        if (len <= 0)
            break;

        node = calloc (sizeof (ParsedTag), 1);
        if (! node)
            return 0;
        node->type = te_name;
        if (! EIP_strdup (&node->value.name, tag, len))
            return 0;
        append_tag (&tl, node);

        switch (tag[len])
        {
        case '\0':
            return tl;
        case '.':
            tag += len+1;
            break;
        case '[':
            node = calloc (sizeof (ParsedTag), 1);
            if (! node)
                return 0;
            node->type = te_element;
            node->value.element = atol (tag+len+1);
            append_tag (&tl, node);
            tag = strchr (tag+len+1, ']');
            if (tag)
                ++tag;
            else
                return 0;
            break;
        }
    }
    return tl;
}

void EIP_dump_ParsedTag (const ParsedTag *tag)
{
    bool did_first = false;
    while (tag)
    {
        switch (tag->type)
        {
        case te_name:    if (did_first)
                             printf (".");
                         printf ("%s", tag->value.name);
                         break;
        case te_element: printf ("[%d]", tag->value.element);
                         break;
        }
        tag = tag->next;
        did_first = true;
    }
    printf ("\n");
}

void EIP_free_ParsedTag (ParsedTag *tag)
{
    ParsedTag *tmp;

    while (tag)
    {
        if (tag->type == te_name)
           free (tag->value.name);
        tmp = tag;
        tag = tag->next;
        free (tmp);
    }
}

/* Word-size of path for ControlLogix tag */
static size_t tag_path_size (const ParsedTag *tag)
{
    size_t bytes = 0, slen;
    
    while (tag)
    {
        switch (tag->type)
        {
        case te_name:
            slen = strlen(tag->value.name);
            bytes += 2 + slen + slen%2;    /* 0x91, len, strting [, pad] */
            break;
        case te_element:
            if (tag->value.element <= 0xFF)
                bytes += 2;
            else
            if (tag->value.element <= 0xFFFF)
                bytes += 4;
            else
                bytes += 6;
            break;
        }
        tag = tag->next;
    }

    return bytes / 2;
}

/* build path for ControlLogix tag */
static CN_USINT *make_tag_path (CN_USINT *path, const ParsedTag *tag)
{   
    size_t slen;

    while (tag)
    {
        switch (tag->type)
        {
        case te_name:
            slen = strlen(tag->value.name);

            path[0] = 0x91; /* spec 4 p.21: "ANSI extended symbol segment" */
            path[1] = (CN_USINT)slen;
            memcpy (& path[2],  tag->value.name, slen);
            if (slen % 2) /* pad */
                path[2+slen] = 0;
            path += 2 + slen + slen%2;
            break;
        case te_element:
            if (tag->value.element <= 0xFF)
            {
                *(path++) = 0x28;
                *(path++) = tag->value.element;
            }
            else
            if (tag->value.element <= 0xFFFF)
            {
                *(path++) = 0x29;
                *(path++) = 0x00;
                *(path++) =  tag->value.element & 0x00FF;
                *(path++) = (tag->value.element & 0xFF00) >> 8;
            }
            else
            {
                *(path++) = 0x2A;
                *(path++) = 0x00;
                *(path++) =  tag->value.element & 0x000000FF;
                *(path++) = (tag->value.element & 0x0000FF00) >> 8;
                *(path++) = (tag->value.element & 0x00FF0000) >> 16;
                *(path++) = (tag->value.element & 0xFF000000) >> 24;
            }
            break;
        }
        tag = tag->next;
    }

    return path;
}

static const CN_USINT *dump_raw_path (CN_USINT size, const CN_USINT *path)
{
    size_t i;
    CN_UINT  vi;
    CN_UDINT vd;

    size *= 2; /* word len -> byte len */
    while (size > 0)
    {
        switch (path[0])
        {
            case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
            case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
            case 0x0B: case 0x0C: case 0x0D: case 0x0E:
                EIP_printf (0, "Port %d, link %d ", path[0], path[1]);
                path += 2;
                size -= 2;
                break;
            case 0x20:
                EIP_printf (0, "Class 0x%02X (%s) ",
                            path[1], EIP_class_name (path[1]));
                path += 2;
                size -= 2;
                break;
            case 0x24:
                EIP_printf (0, "Inst. %d ", path[1]);
                path += 2;
                size -= 2;
                break;
            case 0x30:
                EIP_printf (0, "Attr. %d ", path[1]);
                path += 2;
                size -= 2;
                break;
            case 0x91:
                EIP_printf (0, "'");
                for (i=0; i<path[1]; ++i)
                    EIP_printf (0, "%c", path[2+i]);
                EIP_printf (0, "'");
                i = 2+path[1]; /* taglen */
                if (i%2)
                    ++i;
                path += i;
                size -= i;
                break;
            case 0x28:
                EIP_printf (0, "Element %d", path[1]);
                path += 2;
                size -= 2;
                break;
            case 0x29:
                unpack_UINT (path+2, &vi);
                EIP_printf (0, "Element %d", vi);
                path += 4;
                size -= 4;
                break;
            case 0x2A:
                unpack_UDINT (path+2, &vd);
                EIP_printf (0, "Element %d", vd);
                path += 6;
                size -= 6;
                break;
            default:
                EIP_printf (0, "<unknown>");
                size = 0;
        }
    }
    EIP_printf (0, "\n");

    return path;
}

/********************************************************
 * Message Router: PDU (Protocol Data Unit)
 ********************************************************/

static const char *service_name (CN_Services service)
{
    switch (service)
    {
    case S_Get_Attribute_All:       return "Get_Attribute_All";
    case S_Get_Attribute_Single:    return "Get_Attribute_Single";
    case S_CIP_MultiRequest:        return "S_CIP_MultiRequest";
    case S_CIP_ReadData:            return "CIP_ReadData";
    case S_CIP_WriteData:           return "CIP_WriteData";
    case S_CM_Unconnected_Send:     return "CM_Unconnected_Send";
    case S_CM_Forward_Open:         return "CM_Forward_Open";
    default:                        return "<unknown>";
    }
}

static const char *CN_error_text (CN_USINT status)
{
    /* Spec 4, p.46 and 1756-RM005A-EN-E */
    switch (status)
    { 
    case 0x00:  return "Ok";
    case 0x04:  return "Unknown tag or Path error";
    case 0x05:  return "Instance not found";
    case 0x06:  return "Buffer too small, partial data only";
    case 0x08:  return "Service not supported";
    case 0x09:  return "Invalid Attribute";
    case 0x13:  return "Not enough data";
    case 0x14:  return "Attribute not supported, ext. shows attribute";
    case 0x15:  return "Too much data";
    case 0x1E:  return "One of the MultiRequests stinks";
    }
    return "<unknown>";
}

static size_t MR_Request_size (size_t path_size /* in words */) 
{   /* In Bytes */
    return 2*sizeof(CN_USINT) + path_size*2;
}

/* Setup packed MR_Request, return location of path in there */
static CN_USINT *make_MR_Request (CN_USINT *buf,
                                  CN_USINT service, CN_USINT path_size)
{
    buf = pack_USINT (buf, service);
    return pack_USINT (buf, path_size);
}

/* Get pointer to data section of raw MR_Request
 * (stuff that follows the path) */
static CN_USINT *raw_MR_Request_data (CN_USINT *request)
{
    return request + 2 + /*path_size */ request[1] * 2;
}

static const CN_USINT *dump_raw_MR_Request (const CN_USINT *request)
{
    CN_USINT        service   = request[0];
    CN_USINT        path_size = request[1];
    const CN_USINT *path      = request+2;
    
    EIP_printf (0, "MR_Request\n");
    EIP_printf (0, "    USINT service   = 0x%02X = %s\n",
                service, service_name(service));
    EIP_printf (0, "    USINT path_size = %d\n", path_size);
    EIP_printf (0, "          path      = ");
    return dump_raw_path (path_size, path);
}

/* MR_Response has fixed portion followed by (maybe) an extended
 * status and then (maybe) data.
 * This routine takes the raw MR_Response (as in the receive buffer),
 * gets pointer to data & fills data_size.
 *
 * If data_len==0, the result is still the location immediately
 * after this response
 */
CN_USINT *EIP_raw_MR_Response_data (const CN_USINT *response,
                                    size_t response_size,
                                    size_t *data_size)
{
    CN_UINT *data = (CN_UINT *) (response + 4);
    size_t  non_data;

    /* extended_status_size is CN_USINT, no need to unpack: */
    if (response[3] > 0)
        data += response[3]; /* word ptr ! */

    if (data_size)
    {
        non_data = (char *)data - (char *)response;
        if (response_size > non_data)
            *data_size = response_size - non_data;
        else
            *data_size = 0;
    }

    return (CN_USINT *)data;
}

static const CN_USINT *dump_raw_MR_Response (const CN_USINT *response,
                                             size_t response_size)
{
    size_t         data_len;
    CN_UINT        ext;
    CN_USINT       service, reserved, general_status, extended_status_size;
    const CN_USINT *data, *ext_buf;

    service              = response[0];
    reserved             = response[1];
    general_status       = response[2];
    extended_status_size = response[3];
    ext_buf              = response+4;
                      
    EIP_printf (0, "MR_Response:\n");
    EIP_printf (0, "    USINT service         = 0x%02X = Response to %s\n",
                service, service_name(service & 0x7F));
    EIP_printf (0, "    USINT reserved        = 0x%02X\n", reserved);
    EIP_printf (0, "    USINT status          = 0x%02X (%s)\n",
                general_status,	CN_error_text (general_status));
    EIP_printf (0, "    USINT ext. stat. size = %d\n", extended_status_size);
    while (extended_status_size > 0)
    {
        unpack_UINT (ext_buf, &ext);
        EIP_printf (0, "    ext. status           = 0x%04X\n", ext);
        if (general_status == 0xFF)
        {
            switch (ext)
            {
                case 0x2105:
                    EIP_printf (0, "    (Access beyond end of object, "
                                "wrong array index)\n");
                    break;
                case 0x2107:
                    EIP_printf (0, "    (CIP type does not match "
                                "object type)\n");
                    break;
                case 0x2104:
                    EIP_printf (0, "    (Beginning offset beyond end "
                                "of template)\n");
                    break;
                case 0x0107:
                    EIP_printf (0, "    (Connection not found)\n");
                    break;
            }
        }
        --extended_status_size;
    }
    /* Could get data pointer from ext. status handling,
     * but this way we debug EIP_MR_Response_data(): */
    data = EIP_raw_MR_Response_data (response, response_size, &data_len);
    if (data_len > 0)
    {
        EIP_printf (0, "    data (net format) =\n    ");
        EIP_hexdump (data, data_len);
    }

    return data + data_len;
}

static bool is_raw_MRResponse_ok (const CN_USINT *response,
                                  size_t response_size)
{
    CN_USINT general_status = response[2]; /* needn't unpack USINT */
    if (general_status == 0)
        return true;

    if (EIP_verbosity >= 2)
        dump_raw_MR_Response (response, response_size);

    return false;
}

/********************************************************
 * Connection Manager
 ********************************************************/

/* seperate time in millisecs
 * into time-per-tick value
 * and number of ticks (see Spec 4 p. 30,31)
 */
static bool calc_tick_time (size_t millisec, CN_USINT *tick_time, CN_USINT *ticks)
{
    if (millisec > 8355840)
        return false;

    *tick_time = 0;
    while (millisec > 0xFF)
    {
        ++*tick_time;
        millisec >>= 1;
    }
    *ticks = millisec;

    return true;
}

/********************************************************
 * Support for CM_Unconnected_Send
 * via ConnectionManager in ENET module
 * to ControlLogix PLC, path 01 00
 *
 * Spec 4, p 41,  EMail from Pyramid solutions
 ********************************************************/

size_t CM_Unconnected_Send_size (size_t message_size)
{
    return MR_Request_size (CIA_path_size (C_ConnectionManager, 1, 0))
    + sizeof (CN_USINT)                /* priority_and_tick */
    + sizeof (CN_USINT)                /* connection_timeout_ticks */
    + sizeof (CN_UINT)                 /* message_size */
    + message_size + message_size % 2  /* padded */
    + 4;                               /* complete path to PLC */
}

/* Fill MR_Request with Unconnected_Send for message of given size,
 * return pointer to message location in there
 * (to be filled, that's a nested, DIFFERENT request!)
 * or 0 on error
 */
CN_USINT *make_CM_Unconnected_Send (CN_USINT *request, size_t message_size)
{
    CN_USINT *buf, *nested_request;
    CN_USINT tick_time, ticks;

    calc_tick_time (245760, &tick_time, &ticks);
    
    buf = make_MR_Request (request,
                           S_CM_Unconnected_Send,
                           CIA_path_size (C_ConnectionManager, 1, 0));
    buf = make_CIA_path (buf, C_ConnectionManager, 1, 0);
    
    buf = pack_USINT (buf, tick_time);
    buf = pack_USINT (buf, ticks);
    buf = pack_UINT  (buf, message_size);
    nested_request = buf;
    buf += message_size + message_size%2;
    buf = pack_USINT (buf, port_path_size (1, 0));
    buf = pack_USINT (buf, 0 /* reserved */);
    make_port_path (buf, 1, 0); /* Port 1 = backplane, link 0 (slot 0?) */

    return nested_request;
}

/********************************************************
 * Support for "Logix 5000 Data Access"
 *
 * AB document 1756-RM005A-EN-E
 ********************************************************/

/* Determine byte size of CIP_Type */
size_t CIP_Type_size (CIP_Type type)
{
    switch (type)
    {
        case T_CIP_BOOL:  return sizeof(CN_USINT);
        case T_CIP_SINT:  return sizeof(CN_USINT);
        case T_CIP_INT:   return sizeof(CN_UINT);
        case T_CIP_DINT:  return sizeof(CN_DINT);
        case T_CIP_REAL:  return sizeof(CN_REAL);
        case T_CIP_BITS:  return sizeof(CN_UDINT);
        default:
            return 0;
    }
}

/* MR_Request for S_CIP_ReadData:
 *   MR_Request w/ tag path
 *   CN_UINT    elements;   // number of array elements
 */

static size_t CIP_ReadData_size (const ParsedTag *tag)
{
    return   2                          /* service, path_size */
           + 2 * tag_path_size (tag)    /* IOI path is in words */
           + sizeof (CN_UINT);          /* elements */
}

CN_USINT *make_CIP_ReadData (CN_USINT *request, const ParsedTag *tag, size_t elements)
{
    CN_USINT *buf = make_MR_Request (request, S_CIP_ReadData, tag_path_size (tag));
    buf = make_tag_path (buf, tag);
    return pack_UINT (buf, elements);
}

static const CN_USINT *dump_raw_CIP_ReadDataRequest (const CN_USINT *request)
{
    const CN_USINT *buf;
    CN_UINT  els;
    EIP_printf (0, "CIP ReadData, ");
    buf = dump_raw_MR_Request (request);
    buf = unpack_UINT (buf, &els);
    EIP_printf (0, "    UINT elements = %d\n", els);
    return buf;
}

/* MR_Response for S_CIP_ReadData:
 *  MR_Response
 *  CN_USINT   abbreviated type, data
 */

/* dump CIP data, type and data are in raw format */
void dump_raw_CIP_data (const CN_USINT *raw_type_and_data, size_t elements)
{
    CN_UINT        type;
    const CN_USINT *buf;
    size_t         i;
    CN_USINT       vs;
    CN_UINT        vi;
    CN_UDINT       vd;
    CN_REAL        vr;

    buf = unpack_UINT (raw_type_and_data, &type);
    switch (type)
    {
        case T_CIP_BOOL:
            printf ("BOOL");
            for (i=0; i<elements; ++i)
            {
                vs = *(buf++);
                printf (" %d", (int)vs);
            }
            break;
        case T_CIP_SINT:
            printf ("SINT");
            for (i=0; i<elements; ++i)
            {
                vs = *(buf++);
                printf (" %d", (int)vs);
            }
            break;
        case T_CIP_INT:
            printf ("INT");
            for (i=0; i<elements; ++i)
            {
                buf = unpack_UINT (buf, &vi);
                printf (" %d", (int)vi);
            }
            break;
        case T_CIP_DINT:
            printf ("DINT");
            for (i=0; i<elements; ++i)
            {
                buf = unpack_UDINT (buf, &vd);
                printf (" %d", (int)vd);
            }
            break;
        case T_CIP_REAL:
            printf ("REAL");
            for (i=0; i<elements; ++i)
            {
                buf = unpack_REAL (buf, &vr);
                printf (" %f", (double)vr);
            }
            break;
        case T_CIP_BITS:
            printf ("BITS");
            for (i=0; i<elements; ++i)
            {
                buf = unpack_UDINT (buf, &vd);
                printf (" 0x%08X", (unsigned int)vd);
            }
            break;
        default:
            printf ("raw CIP data, unknown type 0x%04X: ",
                    type);
            EIP_hexdump (buf, elements*CIP_Type_size (type));
    }
    printf ("\n");
}

bool get_CIP_double (const CN_USINT *raw_type_and_data,
                     size_t element, double *result)
{
    CN_UINT        type;
    const CN_USINT *buf;
    CN_USINT       vs;
    CN_UINT        vi;
    CN_UDINT       vd;
    CN_REAL        vr;

    buf = unpack_UINT (raw_type_and_data, &type);
    /* buf now on first, skip to given element */
    if (element > 0)
        buf += element*CIP_Type_size (type);
    switch (type)
    {
        case T_CIP_BOOL:
        case T_CIP_SINT:
            vs = *(buf++);
            *result = (double) vs;
            return true;
        case T_CIP_INT:
            unpack_UINT (buf, &vi);
            *result = (double) vi;
            return true;
        case T_CIP_DINT:
        case T_CIP_BITS:
            unpack_UDINT (buf, &vd);
            *result = (double) vd;
            return true;
        case T_CIP_REAL:
            unpack_REAL (buf, &vr);
            *result = (double) vr;
            return true;
    }
    return false;
}

bool get_CIP_UDINT (const CN_USINT *raw_type_and_data,
                    size_t element, CN_UDINT *result)
{
    CN_UINT        type;
    const CN_USINT *buf;
    CN_USINT       vs;
    CN_UINT        vi;
    CN_REAL        vr;

    buf = unpack_UINT (raw_type_and_data, &type);
    buf += element*CIP_Type_size (type);
    switch (type)
    {
        case T_CIP_BOOL:
        case T_CIP_SINT:
            vs = *buf;
            *result = (CN_UDINT) vs;
            return true;
        case T_CIP_INT:
            unpack_UINT (buf, &vi);
            *result = (CN_UDINT) vi;
            return true;
        case T_CIP_DINT: 
        case T_CIP_BITS:
            unpack_UDINT (buf, result);
            return true;
        case T_CIP_REAL:
            unpack_REAL (buf, &vr);
            *result = (CN_UDINT) vr;
            return true;
    }
    return false;
}

bool put_CIP_double (const CN_USINT *raw_type_and_data,
                     size_t element, double value)
{
    CN_UINT   type;
    CN_USINT *buf;

    buf = (CN_USINT *) unpack_UINT (raw_type_and_data, &type);
    /* buf now on first, skip to given element */
    if (element > 0)
        buf += element*CIP_Type_size (type);
    switch (type)
    {
        case T_CIP_BOOL:
        case T_CIP_SINT:
            pack_USINT (buf, (CN_USINT) value);
            return true;
        case T_CIP_INT:
            pack_UINT (buf, (CN_INT) value);
            return true;
        case T_CIP_DINT:
        case T_CIP_BITS:
            pack_UDINT (buf, (CN_DINT) value);
            return true;
        case T_CIP_REAL:
            pack_REAL (buf, (CN_REAL) value);
            return true;
    }
    return false;
}

bool put_CIP_UDINT (const CN_USINT *raw_type_and_data,
                    size_t element, CN_UDINT value)
{
    CN_UINT   type;
    CN_USINT *buf;

    EIP_printf (8, "put_CIP_UDINT 0x%0X @ %d\n",
                value, element);
    
    buf = (CN_USINT *) unpack_UINT (raw_type_and_data, &type);
    /* buf now on first, skip to given element */
    if (element > 0)
        buf += element*CIP_Type_size (type);
    switch (type)
    {
        case T_CIP_BOOL:
        case T_CIP_SINT:
            pack_USINT (buf, (CN_USINT) value);
            return true;
        case T_CIP_INT:
            pack_UINT (buf, (CN_UINT) value);
            return true;
        case T_CIP_DINT:
        case T_CIP_BITS:
            pack_UDINT (buf, value);
            return true;
        case T_CIP_REAL:
            pack_REAL (buf, (CN_REAL) value);
            return true;
    }
    return false;
}


/* Test CIP_ReadData response, returns data and fills data_size if so */
const CN_USINT *check_CIP_ReadData_Response (const CN_USINT *response,
                                             size_t response_size,
                                             size_t *data_size)
{
    CN_USINT service = response[0];
    
    if ((service & 0x7F) == S_CIP_ReadData &&
        is_raw_MRResponse_ok (response, response_size))
        return EIP_raw_MR_Response_data (response, response_size, data_size);
    return 0;
}

/* MR_Request for S_CIP_WriteData:
 *   MR_Request
 *   CN_UINT    abbreviated_type; // for atomic types
 *   CN_UINT    elements;         // number of array elements
 *   CN_???     data;
 */
static size_t CIP_WriteData_size (const ParsedTag *tag, size_t data_size)
{
    return   2
           + 2 * tag_path_size (tag) /* IOI path is in words */
           + 4 + data_size;
}

/* Fill buffer with CIP WriteData request
 * for tag, type of CIP data, given number of elements.
 * Also copies data into buffer,
 * data has to be in network format already!
 */
CN_USINT *make_CIP_WriteData (CN_USINT *buf, const ParsedTag *tag,
                              CIP_Type type, size_t elements,
                              CN_USINT *raw_data)
{
    size_t data_size = CIP_Type_size (type) * elements;
    buf = make_MR_Request (buf, S_CIP_WriteData, tag_path_size (tag));
    buf = make_tag_path (buf, tag);
    buf = pack_UINT (buf, type);
    buf = pack_UINT (buf, elements);
    memcpy (buf, raw_data, data_size);
    return buf + data_size;
}

void dump_CIP_WriteRequest (const CN_USINT *request)
{
    const CN_USINT *buf;
    CN_UINT type, elements;
    
    buf = dump_raw_MR_Request (request);
    buf = unpack_UINT (buf, &type);
    buf = unpack_UINT (buf, &elements);
    EIP_printf (0, "    UINT CIP type   = 0x%02X\n", type);
    EIP_printf (0, "    UINT elements   = %d\n", elements);
    EIP_printf (0, "    raw data        =\n");
    EIP_hexdump (buf, elements *  CIP_Type_size (type));
}

/* Test CIP_WriteData response: If not OK, report error */
static bool check_CIP_WriteData_Response (const CN_USINT *response,
                                          size_t response_size)
{
    CN_USINT service = response[0];
    return (service & 0x7F) == S_CIP_WriteData &&
        is_raw_MRResponse_ok (response, response_size);
}

/* CIP_MultiRequest:
 *  MR_Request
 *  CN_UINT    count      number of requests that follow
 *  CN_UINT    offset[0]  byte offset to Nth request from &count
 *  CN_UINT    offset[..count-1]
 *  service #0
 *  service #1 ...
 */

/* Determine byte-size of CIP_MultiRequest.
 * count        : # of requests that follow
 * requests_size: total size of all the following requests
 */
size_t CIP_MultiRequest_size (size_t count, size_t requests_size)
{
    return   2                          /* service, path_size */
           + CIA_path_size (C_MessageRouter, 1, 0)*2 /* into bytes */
           + 2 + 2 * count              /* count, offsetfields */
           + requests_size;
}

/* Initialize an MR_Request with a CIP_MultiRequest service
 * for count single requests.
 * Has to be followed by calls to CIP_MultiRequest_item
 * for completion.
 */
bool prepare_CIP_MultiRequest (CN_USINT *request, size_t count)
{
    CN_USINT *buf;
    size_t i;

    buf = make_MR_Request (request, S_CIP_MultiRequest,
                           CIA_path_size (C_MessageRouter, 1, 0));
    buf = make_CIA_path (buf, C_MessageRouter, 1, 0);
    buf = pack_UINT (buf, count);
    
    /* offset is from "count" field, 2 bytes per word ! */
    buf = pack_UINT (buf, (count+1) * 2); /* offset[0] */
    for (i=1; i<count; ++i)
        buf = pack_UINT (buf, 0); /* offset[i] */

    return true;
}

/* To be called with request_no=0, 1, 2, .... (count-1) in that order!
 *
 * request_no         : number of current request item
 * single_request_size: size of current request
 *
 * Returns pointer into MultiRequest were single request can be placed
 */
CN_USINT *CIP_MultiRequest_item (CN_USINT *request,
                                 size_t request_no,
                                 size_t single_request_size)
{
    CN_USINT *countp = raw_MR_Request_data (request);
    CN_USINT *offsetp, *item;
    CN_UINT  count, offset, next_no;

    offsetp = (CN_USINT *)unpack_UINT (countp, &count);
    if (request_no >= count)
    {
        EIP_printf (2, "CIP_MultiRequest_item: item #%d > count (%d)\n",
            request_no, count);
        return 0;
    }

    unpack_UINT (offsetp + 2*request_no, &offset);
    if (offset == 0)
    {
        EIP_printf (2, "CIP_MultiRequest_item (request_no %d): "
                    "not called in order\n",
                    request_no);
        return 0;
    }
    item = countp + offset;
    next_no = request_no+1; /* place following message behind this one,*/
    if (next_no < count)    /* get byte-offset from "count"            */
        pack_UINT (offsetp+2*next_no, offset + single_request_size);

    return item;
}

/* Estimated byte-size of a MultiRequest response:
 * MR_Response
 * UINT count
 * UINT offset[0]
 * UINT offset[1]
 * UINT offset[count-1]
 * response 0 ...
 */
size_t CIP_MultiResponse_size (size_t count, size_t responses_size)
{
    return   4                          /* service, 0, stat, 0 */
           + 2 + 2 * count              /* count, offsetfields */
           + responses_size;
}

/* Check if response is valid for a S_CIP_MultiRequest */
bool check_CIP_MultiRequest_Response (const CN_USINT *response)
{
    CN_USINT service        = response[0];
    CN_USINT general_status = response[2];
    return service == (S_CIP_MultiRequest|0x80)  &&  general_status == 0;
}

/* Pick a single reply out of the muliple reply */
const CN_USINT *get_CIP_MultiRequest_Response (const CN_USINT *response,
                                               size_t response_size,
                                               size_t reply_no,
                                               size_t *reply_size)
{
    const CN_USINT *countp, *offsetp, *mem;
    CN_UINT count, offset, offset2;

    countp = EIP_raw_MR_Response_data (response, response_size, 0);
    offsetp = unpack_UINT (countp, &count);
    if (reply_no >= count)
        return 0;
    unpack_UINT (offsetp + 2*reply_no, &offset);
    mem = countp + offset;
    if (reply_size)
    {
        /* size: from this message to next message */
        if (reply_no+1 < count)
        {
            unpack_UINT (offsetp + 2*(reply_no+1), &offset2);
            *reply_size = offset2 - offset;
        }
        else
        {
            /*from last message to end, mem is on last response item */
            *reply_size = response_size - (mem - response);
        }
    }
    
    return mem;
}

/********************************************************
 * Connection: socket, connect, send/receive buffers, ...
 ********************************************************/

void EIP_dump_connection (const EIPConnection *c)
{
    printf ("EIPConnection:\n");
    printf ("    SOCKET          : %d\n", c->sock);
    printf ("    millisec_timeout: %d\n", c->millisec_timeout);
    printf ("    CN_UDINT session: 0x%08lX\n", c->session);
    printf ("    buffer size     : %d\n", c->size);
    printf ("    buffer location : 0x%08lX\n", (unsigned long)c->buffer);
}

/* set socket to non-blocking */
static void set_nonblock (SOCKET s, bool nonblock)
{
    int yesno = nonblock;
#ifdef _WIN32
    ioctlsocket (s, FIONBIO, &yesno);
#else
#ifdef vxWorks
    ioctl (s, FIONBIO, yesno);
#else
    fcntl (s, FNDELAY, yesno);
#endif
#endif
}

#ifndef vxWorks
/* vxWorks defines these convenient calls,
 * Unix and WIN32 don't
 *
 * Return codea ala vxWorks: OK == 0, ERROR == -1
 */
static int connectWithTimeout (SOCKET s, const struct sockaddr *addr,
                               int addr_size,
                               struct timeval *timeout)
{
    fd_set fds;
    int error;
    
    set_nonblock (s, true);
    if (connect (s, addr, addr_size) == SOCKET_ERROR)
    {
        error = SOCKERRNO;
        if (error == SOCK_EWOULDBLOCK || error == SOCK_EINPROGRESS)
        {
            /* Wait for connection until timeout:
             * success is reported in writefds */
            FD_ZERO (&fds);
            FD_SET (s, &fds);
            if (select (s+1, 0, &fds, 0, timeout) > 0)
                goto got_connected;
        }
        return -1;
    }
  got_connected:
    set_nonblock (s, false);
    
    return 0;
}

static unsigned long hostGetByName (const char *ip_addr)
{
    struct hostent *hostent;
    
    hostent = gethostbyname (ip_addr);
    if (!hostent)
        return -1;
    return *((unsigned long *) hostent->h_addr);
}

#endif

/* Init. connection:
 * Init. fields,
 * connect to target
 */
static bool EIP_init_and_connect (EIPConnection *c,
                                  const char *ip_addr, unsigned short port,
                                  size_t millisec_timeout)
{
    struct sockaddr_in addr;
    struct timeval timeout;
    unsigned long *addr_p;
                
    memset (c, 0, sizeof (EIPConnection));
    c->transfer_buffer_limit = 500;
    c->millisec_timeout = millisec_timeout;
    c->sock = 0;
    timeout.tv_sec = millisec_timeout/1000;
    timeout.tv_usec = (millisec_timeout-timeout.tv_sec*1000)*1000;

    /* Get IP from ip_addr in '123.456.789.123' format ... */
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (port);
#ifdef _WIN32
    addr_p = &addr.sin_addr.S_un.S_addr;
#else
    addr_p =(unsigned long *) &addr.sin_addr.s_addr;
#endif
    *addr_p = inet_addr (ip_addr);
    if (*addr_p == INADDR_NONE)
    {   /* ... or DNS */
        *addr_p = hostGetByName ((char *)ip_addr);
        if (*addr_p == -1)
        {
            EIP_printf (2, "EIP cannot find IP for '%s'\n",
                        ip_addr);
            return false;
        }
    }

    /* Create socket */
    c->sock = socket (AF_INET, SOCK_STREAM, 0);
    if (c->sock == INVALID_SOCKET)
    {
        EIP_printf (2, "EIP cannot create socket\n");
        c->sock = 0;
        return false;
    }

    if (connectWithTimeout (c->sock, (struct sockaddr *)&addr,
                            sizeof (addr), &timeout) != 0)
    {
        EIP_printf (3, "EIP cannot connect to %s:0x%04X\n", ip_addr, port);
        socket_close (c->sock);
        c->sock = 0;
        return false;
    }

    EIP_printf (9, "EIP connected to %s:0x%04X on socket %d\n",
                ip_addr, port, c->sock);

    return true;
}

static void EIP_disconnect (EIPConnection *c)
{
    EIP_printf (9, "EIP disconnecting socket %d\n", c->sock);

    socket_close (c->sock);
    c->sock = 0;
}

/* Assert that *buffer can hold "requested" bytes.
 * A bit like realloc, but only grows and keeps old buffer
 * if no more space */
bool EIP_reserve_buffer (void **buffer, size_t *size, size_t requested)
{
    void *old_buffer;
    int  old_size;

    if (*size >= requested)
        return true;

    old_buffer = *buffer;
    old_size   = *size;
    *buffer = malloc (requested);
    if (! *buffer)
    {
        *buffer = old_buffer;
        return false;
    }
    
    *size = requested;
    if (old_buffer)
    {
        memcpy (*buffer, old_buffer, old_size);
        free (old_buffer);
    }
    return true;
}

bool EIP_send_connection_buffer (EIPConnection *c)
{
    CN_UINT length;
    int     len;
    
    unpack_UINT (c->buffer+2, &length);
    len = sizeof_EncapsulationHeader + length;
    return send (c->sock, (void *)c->buffer, len, 0) == len;
}

bool EIP_read_connection_buffer (EIPConnection *c)
{
    int got = 0;
    bool checked = false;
    bool ok = true;
    int part, needed;
    fd_set fds;
    struct timeval timeout;
    CN_UINT length;
    
    set_nonblock (c->sock, 1);
    timeout.tv_sec = c->millisec_timeout/1000;
    timeout.tv_usec = (c->millisec_timeout - timeout.tv_sec*1000)*1000;
    do
    {
        FD_ZERO (&fds);
        FD_SET (c->sock, &fds);
        if (select (c->sock+1, &fds, 0, 0, &timeout) <= 0)
        {
            ok = false;
            break;
        }
        
        part = recv (c->sock, ((char *)c->buffer + got), c->size - got, 0);
        if (part == SOCKET_ERROR  || part == 0)
        {
            ok = false;
            break;
        }
        got += part;

        /* check for room for complete message */
        if (got >= sizeof (EncapsulationHeader) && !checked)
        {
            unpack_UINT (c->buffer+2, &length);
            needed = sizeof_EncapsulationHeader + length;
            if (needed > c->size  &&
                !EIP_reserve_buffer ((void**)&c->buffer, &c->size, needed))
            {
                EIP_printf (2, "EIP cannot allocate %d bytes "
                            "for receive buffer\n",
                            needed);
                ok = false;
                break;
            }
            checked = true;
        }
    }
    while (got < sizeof_EncapsulationHeader  ||  got < needed);
    set_nonblock (c->sock, 0);

    return ok;
}

/********************************************************
 * Ethernet Encapsulation
 * Spec 4. pp 154
 ********************************************************/

/* Fill EncapsulationHeader of connection with
 * given command parameters
 * AND reserve enough space for the following
 * command data as specified by "length"
 */
static CN_USINT *make_EncapsulationHeader (EIPConnection *c, CN_UINT command,
                                           CN_UINT length, CN_UDINT options)
{
    CN_USINT *buf;
    
    if (! EIP_reserve_buffer ((void **)&c->buffer, &c->size,
                              sizeof (EncapsulationHeader) + length))
    {
        EIP_printf (1, "EIP make_EncapsulationHeader: "
                    "no memory for %d bytes\n",
                    sizeof (EncapsulationHeader) + length);
        return false;
    }
    buf = c->buffer;
    buf = pack_UINT (buf, command);
    buf = pack_UINT (buf, length);
    buf = pack_UDINT (buf, c->session);
    buf = pack_UDINT (buf, 0);
    buf = pack_USINT (buf, 'A');
    buf = pack_USINT (buf, 'I');
    buf = pack_USINT (buf, 'R');
    buf = pack_USINT (buf, 'P');
    buf = pack_USINT (buf, 'L');
    buf = pack_USINT (buf, 'A');
    buf = pack_USINT (buf, 'N');
    buf = pack_USINT (buf, 'E');
    buf = pack_UDINT (buf, options);
    return buf;
}

/* Unpack from network buffer */
static const CN_USINT *unpack_EncapsulationHeader (const CN_USINT *buf,
                                                   EncapsulationHeader *header)
{
    return unpack (buf, "iiddssssssssd",
                   &header->command,
                   &header->length,
                   &header->session,
                   &header->status,
                   &header->server_context[0],
                   &header->server_context[1],
                   &header->server_context[2],
                   &header->server_context[3],
                   &header->server_context[4],
                   &header->server_context[5],
                   &header->server_context[6],
                   &header->server_context[7],
                   &header->options);
}

/* has to be in host format */
static void dump_EncapsulationHeader (const EncapsulationHeader *header)
{
    
    EIP_printf (10, "EncapsulationHeader:\n");
    EIP_printf (10, "    UINT  command   = 0x%02X", header->command);
    switch (header->command)
    {
        case EC_Nop:
            EIP_printf (10, " (Nop)\n");                break;
        case EC_ListInterfaces:
            EIP_printf (10, " (ListInterfaces)\n");     break;
        case EC_RegisterSession:
            EIP_printf (10, " (RegisterSession)\n");    break;
        case EC_UnRegisterSession:
            EIP_printf (10, " (UnRegisterSession)\n");  break;
        case EC_ListServices:
            EIP_printf (10, " (ListServices)\n");       break;
        case EC_SendRRData:
            EIP_printf (10, " (SendRRData)\n");         break;
        case EC_SendUnitData:
            EIP_printf (10, " (SendUnitData)\n");       break;
        default:
            EIP_printf (10, "\n");
    }
    EIP_printf (10, "    UINT  length    = %d \n",    header->length);
    EIP_printf (10, "    UDINT session   = 0x%08X\n", header->session);
    EIP_printf (10, "    UDINT status    = 0x%08X: ", header->status);
    switch (header->status)
    {
    case 0x00:  EIP_printf (10, "OK\n");
                break;
    case 0x01:  EIP_printf (10, "invalid/unsupported command\n");
                break;
    case 0x02:  EIP_printf (10, "no memory on target\n");
                break;
    case 0x03:  EIP_printf (10, "malformed data in request\n");
                break;
    case 0x64:  EIP_printf (10, "invalid session ID\n");
                break;
    case 0x65:  EIP_printf (10, "invalid data length\n");
                break;
    case 0x69:  EIP_printf (10, "unsupported protocol revision\n");
                break;
    default:    EIP_printf (10, "unknown, see page 165 of spec 4\n");
    }
    EIP_printf (10, "    USINT context[8]= '%s'\n",   header->server_context);
    EIP_printf (10, "    UDINT options   = 0x%08X\n", header->options);
}

/* Encapsulation Command "ListServices".
 * Check if CIP is supported.
 */
static bool EIP_list_services (EIPConnection *c)
{
    const CN_USINT *buf;
    ListServicesReply reply;
    int i;
    bool ok = true;

    if (make_EncapsulationHeader (c, EC_ListServices,
                                  0, 0 /* length, options */) == 0)
        return false;
    if (! EIP_send_connection_buffer (c))
    {
        EIP_printf (2, "EIP list_services: send failed\n");
        return false;
    }

    if (EIP_verbosity >= 10)
    {
        EIP_printf (10, "EIP sending ListServices encapsulation command:\n");
        unpack_EncapsulationHeader ((CN_USINT *)c->buffer, &reply.header);
        dump_EncapsulationHeader (&reply.header);
    }

    if (! EIP_read_connection_buffer (c))
    {
        EIP_printf (2, "EIP list_services: No response\n");
        return false;
    }

    buf = unpack_EncapsulationHeader ((CN_USINT *) c->buffer, &reply.header);
    if (reply.header.command != EC_ListServices  ||
        reply.header.status != 0x0)
    {
        EIP_printf (2, "EIP list_services: Invalid response\n");
        dump_EncapsulationHeader (&reply.header);
        return false;
    }

    /* Check response(s) for "CIP PDU" support.   Spec 4 p 170  */
    buf = unpack_UINT (buf, &reply.count);
    EIP_printf (10, "ListServices reply:\n");
    EIP_printf (10, "    UINT count     = %d\n", reply.count);
    for (i=0; i<reply.count; ++i)
    {
        buf = unpack (buf, "iiiissssssssssssssss",
                      &reply.service.type,
                      &reply.service.length,
                      &reply.service.version,
                      &reply.service.flags,
                      &reply.service.name[ 0],
                      &reply.service.name[ 1],
                      &reply.service.name[ 2],
                      &reply.service.name[ 3],
                      &reply.service.name[ 4],
                      &reply.service.name[ 5],
                      &reply.service.name[ 6],
                      &reply.service.name[ 7],
                      &reply.service.name[ 8],
                      &reply.service.name[ 9],
                      &reply.service.name[10],
                      &reply.service.name[11],
                      &reply.service.name[12],
                      &reply.service.name[13],
                      &reply.service.name[14],
                      &reply.service.name[15]);
        
        EIP_printf (10, "    UINT type     = 0x%04X\n",reply.service.type);
        EIP_printf (10, "    UINT length   = %d\n",    reply.service.length);
        EIP_printf (10, "    UINT version  = 0x%04X\n",reply.service.version);
        EIP_printf (10, "    UINT flags    = 0x%04X ", reply.service.flags);
        if (! (reply.service.flags  &  (1<<5)))
        {
            EIP_printf (2, "\nEIP list_services: NO SUPPORT for"
			   " CIP PDU encapsulation.!\n");
            ok = false;
        }
        else
            EIP_printf (10, "(CIP PDU encap.)\n");
        EIP_printf (10, "    USINT name[16]= '%s'\n", reply.service.name);
    }

    return ok;
}

/* Encapsulation Command "Register Session" */
static bool EIP_register_session (EIPConnection *c)
{
    CN_USINT *sbuf;
    const CN_USINT *rbuf;
    RegisterSessionData data;

    sbuf = make_EncapsulationHeader (c, EC_RegisterSession,
                                     sizeof_RegisterSessionData
                                     - sizeof_EncapsulationHeader,
                                     0 /* options */);
    if (! sbuf)
        return false;
    sbuf = pack_UINT (sbuf, /* protocol_version */ 1);
    pack_UINT (sbuf, /* options */ 0);

    if (! EIP_send_connection_buffer (c))
    {
        EIP_printf (2, "EIP register_session: send failed\n");
        return false;
    }
    if (EIP_verbosity >= 10)
    {
        rbuf = unpack_EncapsulationHeader (c->buffer, &data.header);
        unpack (rbuf, "ii", &data.protocol_version, &data.options);
        EIP_printf (10, "EIP register_session sends:\n");
        dump_EncapsulationHeader (&data.header);
        EIP_printf (10, "    UINT  protocol  = %d \n",
                    data.protocol_version);
        EIP_printf (10, "    UINT  options   = %d \n",
                    data.options);
    }
    if (! EIP_read_connection_buffer (c))
    {
        EIP_printf (2, "EIP register_session: no response\n");
        return false;
    }
    unpack_EncapsulationHeader ((CN_USINT *)c->buffer, &data.header);
    if (data.header.command != EC_RegisterSession  ||
        data.header.status  != 0)
    {
        EIP_printf (2, "EIP register_session received error\n");
        if (EIP_verbosity >= 3)
            dump_EncapsulationHeader (&data.header);
        return false;
    }
    c->session = data.header.session; /* keep session ID that target sent */
    EIP_printf (9, "EIP registered session 0x%08X\n", c->session);
    
    return true;
}

/* Encapsulation Command "UnRegister Session" */
static bool EIP_unregister_session (EIPConnection *c)
{
    EncapsulationHeader header;
    
    if (! make_EncapsulationHeader (c, EC_UnRegisterSession,
                                    0, 0 /*length, options*/))
        return false;

    EIP_printf (9, "EIP unregister session 0x%08X\n", c->session);
    if (EIP_verbosity >= 10)
    {
        EIP_printf (10, "sending UnRegisterSession encapsulation command:\n");
        unpack_EncapsulationHeader ((CN_USINT *)c->buffer, &header);
        dump_EncapsulationHeader (&header);
    }
    if (! EIP_send_connection_buffer (c))
        return false;

    return true;
}

/* Setup encapsulation buffer for SendRRData,
 * the unconnected Request/Response command.
 * length: total byte-size of MR_Request
 * Returns pointer to MR_Request to be completed.
 */
CN_USINT *EIP_make_SendRRData (EIPConnection *c, size_t length)
{
    CN_USINT *buf = make_EncapsulationHeader (c, EC_SendRRData,
                                              sizeof_EncapsulationRRData
                                              - sizeof_EncapsulationHeader
                                              + length,
                                              0 /* options */);
    if (!buf)
        return 0;
    buf = pack_UDINT (buf, /* interface_handle */                   0);
    buf = pack_UINT  (buf, /* timeout          */                   0);
    buf = pack_UINT  (buf, /* count (addr., data) */                2);
    buf = pack_UINT  (buf, /* address_type UCMM */               0x00);
    buf = pack_UINT  (buf, /* address_length */                     0);
    buf = pack_UINT  (buf, /* data_type (unconnected message) */ 0xB2);
    buf = pack_UINT  (buf, /* data_length */                   length);
    return buf;
}

/* Unpack reponse to SendRRData.
 * Fills data with details, returns pointer to raw MRResponse
 * that's enclosed in the RRData
 */
const CN_USINT *EIP_unpack_RRData (const CN_USINT *buf,
                                   EncapsulationRRData *data)
{
    buf = unpack_EncapsulationHeader (buf, &data->header);
    if (! buf)
        return 0;
    return unpack (buf, "diiiiii",
                   &data->interface_handle,
                   &data->timeout,
                   &data->count,
                   &data->address_type,
                   &data->address_length,
                   &data->data_type,
                   &data->data_length);
}

/* Send unconnected GetAttributeSingle service request to class/instance/attr
 *
 * Result: ptr to data or 0,
 * len is set to length of data
 */
void *EIP_Get_Attribute_Single (EIPConnection *c,
                                CN_Classes cls, CN_USINT instance,
                                CN_USINT attr, size_t *len)
{
    EncapsulationRRData data;
    size_t         path_size = CIA_path_size (cls, instance, attr);
    size_t         request_size = MR_Request_size (path_size);
    CN_USINT       *request = EIP_make_SendRRData (c, request_size);
    CN_USINT       *path;
    const CN_USINT *response;
    CN_USINT       service;
    CN_USINT       general_status;

    if (! request)
        return 0;
    path = make_MR_Request (request, S_Get_Attribute_Single, path_size);
    make_CIA_path (path, cls, instance, attr);
    if (! EIP_send_connection_buffer (c))
    {
        EIP_printf (2, "EIP_Get_Attribute_Single: send failed\n");
        return 0;
    }
    if (! EIP_read_connection_buffer (c))
    {
        EIP_printf (2, "EIP_Get_Attribute_Single: No response\n");
        return 0;
    }

    response = EIP_unpack_RRData ((CN_USINT *)c->buffer, &data);
    unpack (response, "sSs", &service, &general_status);
    if (service != (S_Get_Attribute_Single | 0x80)  ||
        general_status != 0)
    {
        EIP_printf (2, "EIP_Get_Attribute_Single: error in response\n");
        if (EIP_verbosity >= 3)
            dump_raw_MR_Response (response, data.data_length);
    }

    return EIP_raw_MR_Response_data (response, data.data_length, len);
}

static bool EIP_check_interface (EIPConnection *c)
{
    EIPIdentityInfo  *info = &c->info;
    void *data;
    size_t len;

    data = EIP_Get_Attribute_Single (c, C_Identity, 1, 1, &len);
    if (data && len == sizeof (CN_UINT))
        info->vendor = *((CN_UINT *) data);
    else return false;
    data = EIP_Get_Attribute_Single (c, C_Identity, 1, 2, &len);
    if (data && len == sizeof (CN_UINT))
        info->device_type = *((CN_UINT *) data);
    else return false;
    data = EIP_Get_Attribute_Single (c, C_Identity, 1, 4, &len);
    if (data && len == sizeof (CN_UINT))
        info->revision = *((CN_UINT *) data);
    else return false;
    data = EIP_Get_Attribute_Single (c, C_Identity, 1, 6, &len);
    if (data && len == sizeof (CN_UDINT))
        info->serial_number = *((CN_UDINT *) data);
    else return false;
    data = EIP_Get_Attribute_Single (c, C_Identity, 1, 7, &len);
    if (data && len > 0 && len < 34)
    {
        len = *((CN_USINT *) data);
        memcpy (info->name, (const char *)data+1, len);
        info->name[len] = '\0';
    }
    else return false;
    EIP_printf (9, "Identity information of target:\n");
    EIP_printf (9, "    UINT vendor         = 0x%04X\n", info->vendor);
    EIP_printf (9, "    UINT device_type    = 0x%04X\n", info->device_type);
    EIP_printf (9, "    UINT revision       = 0x%04X\n", info->revision);
    EIP_printf (9, "    UDINT serial_number = 0x%08X\n", info->serial_number);
    EIP_printf (9, "    USINT name          = '%s'\n", info->name);
    return true;
}

bool EIP_startup (EIPConnection *c,
                  const char *ip_addr, unsigned short port,
                  size_t millisec_timeout)
{
    if (! EIP_init_and_connect (c, ip_addr, port, millisec_timeout))
        return false;

    if (! EIP_list_services (c)  ||
        ! EIP_register_session (c))
    {
        EIP_printf (1, "EIP_startup: target %s does not respond\n",
                    ip_addr);
        EIP_disconnect (c);
        return false;
    }

    if (! EIP_check_interface (c))
    {
        /* Warning, ignored */
        EIP_printf (1, "EIP_startup: cannot determine target's identity\n");
    }

    return true;
}

void EIP_shutdown (EIPConnection *c)
{
   EIP_unregister_session (c);
   EIP_disconnect (c);
}


/* The connected methods:
 * Forward_Open, then SendUnitData instead of CM_UnconnectedSend
 * and finally Forward_Close.
 * There wasn't any improved performance, only more headache
 * because of additional timeouts.
 *
 * When an "open" for automatic updated becomes available,
 * this might be interesting again.
 */
#ifdef DEFINE_CONNECTED_METHODS

static void dump_CM_priority_and_tick (CN_USINT pat, CN_UINT ticks)
{
    size_t time = 1 << (pat & 0x0F);
    if (pat & 0x10)
        EIP_printf (0, "High priority for connection request, ");
    EIP_printf (0, " tick time: %d ms, %d ticks = %d ms\n",
		time, ticks, time*ticks);
}

static void dump_CM_Unconnected_Send (const MR_Request *request)
{
    const CM_Unconnected_Send_Request *send_data;
    const CM_Unconnected_Send_Request_2 *send_data2;

    dump_raw_MR_Request (request);

    send_data = (const CM_Unconnected_Send_Request *)
                raw_MR_Request_data (request);
    EIP_printf (0, "    USINT priority_and_tick        = 0x%02X\n",
		send_data->priority_and_tick);
    EIP_printf (0, "    USINT connection_timeout_ticks = %d -> ",
		send_data->connection_timeout_ticks);
    dump_CM_priority_and_tick (send_data->priority_and_tick,
			       send_data->connection_timeout_ticks);
            
    EIP_printf (0, "    UINT  message_size             = %d\n",
		send_data->message_size);
    EIP_printf (0, "    message_router_PDU: ");
    EIP_hexdump (&send_data->message_router_PDU, send_data->message_size);
    dump_raw_MR_Request (&send_data->message_router_PDU);

    send_data2 = (CM_Unconnected_Send_Request_2 *) 
        ( (char *) &send_data->message_router_PDU
           + send_data->message_size + send_data->message_size%2 );
    EIP_printf (0, "    USINT path_size = %d\n", send_data2->path_size);
    EIP_printf (0, "    USINT reserved  = %d\n", send_data2->reserved);
    EIP_printf (0, "          path      = ");
    dump_raw_path (send_data2->path_size, send_data2->path);
}

/********************************************************
 * Support for CM_Forward_Open
 * via ConnectionManager in ENET module
 * to path 01, link 00 and then on to MessageRouter
 *
 * Spec 4, p 37,  EMail from Pyramid solutions
 ********************************************************/

static void dump_CM_connection_parameters (CN_UINT p)
{
    switch ((p >> 13) & 0x03)
    {
    case 0: EIP_printf (0, "Type 0, ");        break;
    case 1: EIP_printf (0, "Multicast, ");     break;
    case 2: EIP_printf (0, "Point2Point, ");   break;
    case 3: EIP_printf (0, "Reserved, ");      break;
    }

    switch ((p >> 10) & 0x03)
    {
    case 0: EIP_printf (0, "Low Priority, ");  break;
    case 1: EIP_printf (0, "High Priority, "); break;
    case 2: EIP_printf (0, "Scheduled, ");     break;
    case 3: EIP_printf (0, "Urgent, ");        break;
    }

    if (p && CM_NCP_Variable)
        EIP_printf (0, " variable sized, ");
    else
        EIP_printf (0, " fixed sized, ");

    EIP_printf (0, "%d  bytes\n", p & 0x01FF);
}

static const char *CM_transport_class[] =
{
    "Base",             /* Send raw data */
    "Duplicate Detect", /* Send data, sequence number */
    "Acknowledged",     /* Send data, sequence number and return ackn. of data */
    "Verified",         /* Send data, sequence number and return ackn. after data is verified */
    "Nonblocking",
    "Nonblocking, fragmenting",
    "Multicast, fragmenting"
};
/* Now what does this mean?
 * Class 2 vs. 3:
 *   Former returns ack. as soon as data reaches target buffer,
 *   latter returns ack. after application has read data from buffer.
 *
 * Ether/IP case:
 * Class 0, 1: UDP
 * Class 2, 3: TCP
 * Class 4, 5, 6: not available w/ IP
 *
 * ControlLogix: ENET supports only Class 3
 */

static void dump_CM_xport_trig (CN_UINT xt)
{
    int cls = xt & 0x0F;
    EIP_printf (0, "Class %d transport ", cls);
    if (cls <= 6)
        EIP_printf (0, "(%s), ", CM_transport_class[cls]);
    else
        EIP_printf (0, "(%s), ", CM_transport_class[0]);
    switch ((xt >> 4) & 0x07)
    {
    case 0:   EIP_printf (0, "cyclic trigger");          break;
    case 1:   EIP_printf (0, "change-of-state trigger"); break;
    case 2:   EIP_printf (0, "application trigger");     break;
    default:  EIP_printf (0, "<undefined> trigger");     break;
    }
    if (xt & CM_Transp_IsServer)
        EIP_printf (0, " (server)");
    EIP_printf (0, "\n");
}

static size_t CM_Forward_Open_size ()
{
    return MR_Request_size (CIA_path_size (C_ConnectionManager, 1, 0))
           + offsetof (CM_Forward_Open_Request, connection_path)
           + 2*(port_path_size (1, 0) + CIA_path_size (C_MessageRouter, 1, 0));
}

/* Fill MR_Request with CM_Forward_Open
 *
 * Depends on CM_Forward_Open_Request having the correct size!
 */
static bool make_CM_Forward_Open (MR_Request *request, EIPConnectionParameters *params)
{
    CM_Forward_Open_Request *open_data;
    size_t pp;

    request->service = S_CM_Forward_Open;
    request->path_size = CIA_path_size (C_ConnectionManager, 1, 0);
    make_CIA_path (request->path, C_ConnectionManager, 1, 0);
    
    open_data = (CM_Forward_Open_Request *) MR_Request_data (request);
    /* could memcpy, but not sure if EIPConnectionParameters will stay as it is */
    open_data->priority_and_tick        = params->priority_and_tick;
    open_data->connection_timeout_ticks = params->connection_timeout_ticks;
    open_data->O2T_CID                  = params->O2T_CID;
    open_data->T2O_CID                  = params->T2O_CID;
    open_data->connection_serial        = params->connection_serial;
    open_data->vendor_ID                = params->vendor_ID;
    open_data->originator_serial        = params->originator_serial;

    open_data->connection_timeout_multiplier = 0;
    /* 511 = 0x1FF is the maximum packet size, otherwise use ForwardOpenEx.
     * Experientation: 508 us maximum for ControlLogix
     */
    open_data->O2T_connection_parameters = CM_NCP_Point2Point | CM_NCP_LowPriority
                                         | CM_NCP_Variable | 508;
    open_data->T2O_connection_parameters = CM_NCP_Point2Point | CM_NCP_LowPriority
                                         | CM_NCP_Variable | 508;
    open_data->O2T_RPI = 10000000L; /* request. pack. interv., microsecs */
    open_data->T2O_RPI = 10000000L;
    open_data->xport_type_and_trigger = CM_Transp_IsServer | 3 | CM_Trig_App;

    /* Port 1 = backplane, link 0 (=slot 0?), then on to MessageRouter. */
    pp = port_path_size (1, 0);
    open_data->connection_path_size = pp +
                                      CIA_path_size (C_MessageRouter, 1, 0);
    make_port_path (open_data->connection_path, 1, 0);
    make_CIA_path (&open_data->connection_path[2*pp], C_MessageRouter, 1, 0);

    return true;
}

static void dump_CM_Forward_Open (const MR_Request *request)
{
    const CM_Forward_Open_Request *open_data;

    dump_raw_MR_Request (request);
    open_data = (const CM_Forward_Open_Request *) MR_Request_data (request);

    EIP_printf (0, "    USINT priority_and_tick             = 0x%02X\n", open_data->priority_and_tick);
    EIP_printf (0, "    USINT connection_timeout_ticks      = %d -> ", open_data->connection_timeout_ticks);
    dump_CM_priority_and_tick (open_data->priority_and_tick, open_data->connection_timeout_ticks);
    EIP_printf (0, "    UDINT O2T_CID                       = 0x%08X\n", open_data->O2T_CID);
    EIP_printf (0, "    UDINT T2O_CID                       = 0x%08X\n", open_data->T2O_CID);
    EIP_printf (0, "    UINT  connection_serial             = 0x%04X\n", open_data->connection_serial);
    EIP_printf (0, "    UINT  vendor_ID                     = 0x%04X\n", open_data->vendor_ID);
    EIP_printf (0, "    UDINT originator_serial             = 0x%08X\n", open_data->originator_serial);
    EIP_printf (0, "    USINT connection_timeout_multiplier = 0x%02X\n", open_data->connection_timeout_multiplier);
    EIP_printf (0, "    USINT reserved[3]\n");
    EIP_printf (0, "    UDINT O2T_RPI                       = %d us\n", open_data->O2T_RPI);
    EIP_printf (0, "    UINT  O2T_connection_parameters     = 0x%04X ", open_data->O2T_connection_parameters);
    dump_CM_connection_parameters (open_data->O2T_connection_parameters);
    EIP_printf (0, "    UDINT T2O_RPI                       = %d us\n", open_data->T2O_RPI);
    EIP_printf (0, "    UINT  T2O_connection_parameters     = 0x%04X ", open_data->T2O_connection_parameters);
    dump_CM_connection_parameters (open_data->T2O_connection_parameters);
    EIP_printf (0, "    USINT xport_type_and_trigger        = 0x%02X ->", open_data->xport_type_and_trigger);
    dump_CM_xport_trig (open_data->xport_type_and_trigger);
    EIP_printf (0, "    USINT connection_path_size          = %d\n", open_data->connection_path_size);
    EIP_printf (0, "    USINT connection_path[2]            = ");
    dump_path (open_data->connection_path_size, open_data->connection_path);
}

static void dump_CM_Forward_Open_Response (const MR_Response *response,
                                           size_t response_size)
{
    const CM_Forward_Open_Good_Response *data;

    if (response->general_status != 0)
    {
        EIP_printf (2, "Error in dump_CM_Forward_Open_Response:\n");
        if (EIP_verbosity >= 2)
            dump_raw_MR_Response (response, response_size);
        return;
    }

    if (EIP_verbosity >= 10)
    {
        dump_raw_MR_Response (response, offsetof (MR_Response, response));
        data = (CM_Forward_Open_Good_Response *)
               EIP_MR_Response_data (response, response_size, 0);
        EIP_printf (10, "Forward_Open_Response:\n");
        EIP_printf (10, "    UDINT O2T_CID                       = 0x%08X\n",
		    data->O2T_CID);
        EIP_printf (10, "    UDINT T2O_CID                       = 0x%08X\n",
		    data->T2O_CID);
        EIP_printf (10, "    UINT  connection_serial             = 0x%04X\n",
		    data->connection_serial);
        EIP_printf (10, "    UINT  vendor_ID                     = 0x%04X\n",
		    data->vendor_ID);
        EIP_printf (10, "    UDINT originator_serial             = 0x%08X\n",
		    data->originator_serial);
        EIP_printf (10, "    UDINT O2T_API                       = %d us\n",
		    data->O2T_API);
        EIP_printf (10, "    UDINT T2O_API                       = %d us\n",
		    data->T2O_API);
        EIP_printf (10, "    USINT application_reply_size        = %d\n",
		    data->application_reply_size);
        EIP_printf (10, "    USINT application_reply[]           = ");
        EIP_hexdump (&data->application_reply, data->application_reply_size*2);
    }
}

/* Setup encapsulation buffer for SendUnitData,
 * the connected Request command.
 * PDU_size: total byte-size of MR_Request, EXCLUDING sequence Nr.
 * sequence_number: all connected messages have a sequence
 * Returns pointer to MR_Request to be completed.
 */
static MR_Request *make_SendUnitData (EIPConnection *c, size_t PDU_size, CN_UINT sequence_number)
{
    EncapsulationUnitData *buf;

    if (! make_EncapsulationHeader (c, EC_SendUnitData,
                        (CN_UINT)
                         (offsetof(EncapsulationUnitData, rr)
                          - sizeof (EncapsulationHeader)
                          + PDU_size
                         ), 0 /* options */))
        return 0;

    buf = (EncapsulationUnitData *) c->buffer;
    buf->interface_handle = 0;
    buf->timeout = 0;
    buf->count = 2; /* Address & data */
    buf->address_type = 0xA1; /* connected */
    buf->address_length = sizeof (CN_UDINT);
    buf->CID = c->params.O2T_CID;

    buf->data_type = 0xB1; /* connected message */
    buf->data_length = sizeof (CN_UINT) + PDU_size; /* incl. sequence */
    buf->PDU_sequence_number = sequence_number;

    return & buf->rr.request;
}

/********************************************************
 * Support for CM_Forward_Close
 ********************************************************/

static size_t CM_Forward_Close_size ()
{
    return MR_Request_size (CIA_path_size (C_ConnectionManager, 1, 0))
           + offsetof (CM_Forward_Close_Request, connection_path)
           + 2*(port_path_size (1, 0) + CIA_path_size (C_MessageRouter, 1, 0));
}

/* Fill MR_Request with CM_Forward_Close
 *
 * Depends on CM_Forward_Open_Request having the correct size!
 */
static bool make_CM_Forward_Close (MR_Request *request, const EIPConnectionParameters *params)
{
    CM_Forward_Close_Request *close_data;
    size_t pp;

    request->service = S_CM_Forward_Close;
    request->path_size = CIA_path_size (C_ConnectionManager, 1, 0);
    make_CIA_path (request->path, C_ConnectionManager, 1, 0);
    
    close_data = (CM_Forward_Close_Request *) MR_Request_data (request);
    close_data->priority_and_tick        = params->priority_and_tick;
    close_data->connection_timeout_ticks = params->connection_timeout_ticks;
    close_data->connection_serial        = params->connection_serial;
    close_data->vendor_ID                = params->vendor_ID;
    close_data->originator_serial        = params->originator_serial;

    /* Port 1 = backplane, link 0 (=slot 0?), then on to MessageRouter. */
    pp = port_path_size (1, 0);
    close_data->connection_path_size = pp +
                                      CIA_path_size (C_MessageRouter, 1, 0);
    make_port_path (close_data->connection_path, 1, 0);
    make_CIA_path (&close_data->connection_path[2*pp], C_MessageRouter, 1, 0);

    return true;
}

#endif /* DEFINE_CONNECTED_METHODS */

/* Read a single tag in a single CIP_ReadData request,
 * report data & data_length
 * as well as sizes of CIP_ReadData request/response
 */
const CN_USINT *EIP_read_tag (EIPConnection *c,
                              const ParsedTag *tag, size_t elements,
                              size_t *data_size,
                              size_t *request_size, size_t *response_size)
{
    size_t      msg_size = CIP_ReadData_size (tag);
    size_t      send_size = CM_Unconnected_Send_size (msg_size);
    CN_USINT    *send_request, *msg_request;
    const CN_USINT *response, *data;
    EncapsulationRRData rr_data;

    /* Encapsulated for Ethernet in a SendRRData packet:
     * Send CM_Unconnected_Send request with a CIP read inside.
     * Result is the pure CIP read response
     */
    if (request_size)
        *request_size = msg_size;
    send_request = EIP_make_SendRRData (c, send_size);
    if (! send_request)
        return 0;
    msg_request = make_CM_Unconnected_Send (send_request, msg_size);
    if (! msg_request)
        return 0;
    if (! make_CIP_ReadData (msg_request, tag, elements))
        return 0;
    if (EIP_verbosity >= 9)
    {
        EIP_printf (10, "EIP read tag ");
        EIP_dump_ParsedTag (tag);
        if (EIP_verbosity >= 10)
            dump_raw_CIP_ReadDataRequest (msg_request);
    }
    if (! EIP_send_connection_buffer (c))
    {
        EIP_printf (1, "EIP_read_tag: send failed\n");
        return 0;
    }
    if (! EIP_read_connection_buffer (c))
    {
        EIP_printf (1, "EIP_read_tag: No response\n");
        return 0;
    }

    response = EIP_unpack_RRData ((CN_USINT *)c->buffer, &rr_data);
    if (EIP_verbosity >= 10)
        dump_raw_MR_Response (response, rr_data.data_length);
    data = check_CIP_ReadData_Response (response, rr_data.data_length,
                                        data_size);
    if (response_size)
        *response_size = rr_data.data_length;

    if (! data)
    {
        if (EIP_verbosity >= 2)
        {
            EIP_printf (2, "EIP_read_tag: Failed tag: ");
            EIP_dump_ParsedTag (tag);
        }
        return 0;
    }

    return data;
}

/* Write a single tag in a single CIP_ReadData request,
 * report sizes of CIP_WriteData request/response
 */
bool EIP_write_tag (EIPConnection *c, const ParsedTag *tag,
                    CIP_Type type, size_t elements, CN_USINT *data,
                    size_t *request_size,
                    size_t *response_size)
{
    size_t      data_size = CIP_Type_size (type) * elements;
    size_t      msg_size  = CIP_WriteData_size (tag, data_size);
    size_t      send_size = CM_Unconnected_Send_size (msg_size);
    CN_USINT    *send_request, *msg_request;
    const CN_USINT *response;
    EncapsulationRRData rr_data;

    if (request_size)
        *request_size = msg_size;
    send_request = EIP_make_SendRRData (c, send_size);
    if (! send_request)
        return 0;
    msg_request = make_CM_Unconnected_Send (send_request, msg_size);
    if (! msg_request)
        return 0;
    if (! make_CIP_WriteData (msg_request, tag, type, elements, data))
        return 0;
    if (EIP_verbosity >= 9)
    {
        EIP_printf (10, "EIP write tag ");
        EIP_dump_ParsedTag (tag);
        if (EIP_verbosity >= 10)
            dump_raw_MR_Request (msg_request);
    }
    if (! EIP_send_connection_buffer (c))
    {
        EIP_printf (1, "EIP_write_tag: send failed\n");
        return 0;
    }
    if (! EIP_read_connection_buffer (c))
    {
        EIP_printf (1, "EIP_write_tag: No response\n");
        return 0;
    }

    response = EIP_unpack_RRData ((CN_USINT *)c->buffer, &rr_data);
    if (EIP_verbosity >= 10)
        dump_raw_MR_Response (response, rr_data.data_length);

    if (!check_CIP_WriteData_Response (response, rr_data.data_length))
    {
        if (EIP_verbosity >= 2)
        {
            EIP_printf (2, "EIP_write_tag: Failed tag: ");
            EIP_dump_ParsedTag (tag);
        }
        return 0;
    }
    if (response_size)
        *response_size = rr_data.data_length;

    return true;
}

