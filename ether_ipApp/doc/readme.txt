-*- outline -*- $Id$

* This document
is part of the EPICS EtherIP package, to be found in
<ether_ip>/ether_ipApp/doc/readme.txt.
For now this is the "manual" on how to use the driver.

For details on the underlying protocol, see
"Interfacing the ControlLogix PLC Over EtherNet/IP",
 K.U. Kasemir, L.R. Dalesio
 ICALEPCS PSN THAP020
 LANL E-Print Archive: http://arXiv.org/abs/cs.NI/0110065


* EtherNet/IP
EtherNet/IP, originally called "ControlNet over Ethernet"
as defined in the ControlNet Spec, Errata 2, is the protocol
used by Allen-Bradley ControlLogix PLCs.

This software is both a command-line test tool for Win32/Unix
and a driver/device for EPICS IOCs.

* Compilation
See the top-level README

* Command-line tool
The ether_ip_test executable (ether_ip_test.exe on Win32)
allows for simple communication checks.
The available command line options might change,
use the "-?" option for help:
    >ether_ip_test -?
    Usage: ether_ip_test <flags> [tag]
    Options:
      -v verbosity
      -i ip  (as 123.456.789.001 or DNS name)
      -p port
      -s PLC slot in ControlLogix crate (default: 0)
      -s slot (default: 0)
      -t timeout (ms)
      -a array size
      -w <double value to write>       

Example:
Read tag "REAL" from plc with IP 128.165.160.146,
PLC happens to be in slot 6 of the ControlLogix crate:
    >ether_ip_test -i 128.165.160.146 -s 6 REAL
    Tag REAL
    REAL 0.002502

Add the "-v 10" option to see a dump of all the exchanged
EtherIP messages. The included error messages might help
to detect why some tag cannot be read.

* EPICS startup files
1) Load Driver & Device Support
The ether_ipApp creates a library "ether_ipLib"
which contains only the driver code.

You can load the ether_ipLib itself or use e.g.
the makeBaseApp.pl ADE to include this library
in your application library.

2) IP Setup
Since the driver uses TCP/IP, the route to the PLC has to defined.
Therefore you have to add something like this to your vxWorks startup file:
    # Define the DNS name for the PLC, so we can it instead of the
    # raw IP address
    hostAdd "snsplc1", "128.165.160.146"

    # *IF* "128.165.160.146" is in a different subnet
    # that the IOC cannot get to directly, define
    # a route table entry. In this example, ..254 is the gateway
    routeAdd "128.165.160.146", "128.165.160.254"

    # Test: See if the IOC can get to "snsplc1":
    ping "snsplc1", 5

3) Driver Configuration
Before calling iocInit in your vxWorks startup file, the driver has to
be configured. After loading the driver object code either directly
or as part of an ADE library, issue the following commands.
Note that the IP address (128.165.160.146), the DNS name (snsplc1)
and the name that the driver uses (plc1) are all related but different!
    
    # Initialize EtherIP driver, define PLCs
    # -------------------------------------
    drvEtherIP_init

    # Provide a default for the driver's scan rate in case neither
    # the record's SCAN field nor the INP/OUT link
    # contain a scan rate that the driver can use.
    # Recommendation: Do not use this feature, provide a scan flag
    # "S" in the INP/OUT link instead. See manual comments on the "S" flag.
    drvEtherIP_default_rate = 0.5

    # drvEtherIP_define_PLC <name>, <ip_addr>, <slot>
    # The driver/device uses the <name> to indentify the PLC.
    # 
    # <ip_addr> can be an IP address in dot-notation
    # or a name that the IOC knows about (defined via hostAdd).
    # The IP address gets us to the ENET interface.
    # To get to the PLC itself, we need the slot that
    # it resides in. The first, left-most slot in the
    # ControlLogix crate is slot 0.
    # (When omitting the slot number, the default is also 0)
    drvEtherIP_define_PLC "plc1", "snsplc1", 0
       
    # EtherIP driver verbosity, 0=silent, up to 10:
    EIP_verbosity=4

4) Tell EPICS Database about Driver/Device
To inform EPICS of this new driver/device, a DBD file is used.
ether_ip.dbd looks like this:
     driver(drvEtherIP)
     device(ai,         INST_IO, devAiEtherIP,         "EtherIP")
     device(bi,         INST_IO, devBiEtherIP,         "EtherIP")
     device(mbbi,       INST_IO, devMbbiEtherIP,       "EtherIP")
     device(mbbiDirect, INST_IO, devMbbiDirectEtherIP, "EtherIP")
     device(ao,         INST_IO, devAoEtherIP,         "EtherIP")
     device(bo,         INST_IO, devBoEtherIP,         "EtherIP")
     device(mbbo,       INST_IO, devMbboEtherIP,       "EtherIP")
     device(mbboDirect, INST_IO, devMbboDirectEtherIP, "EtherIP")

You can load this directly via dbLoadDatabase in the startup script.
Usually, however, you would use something like the makeBaseApp.pl ADE,
have this:
       include "base.dbd"
       include "ether_ip.dbd"
in your application DBD file and then in the vxWorks startup script,
"dbLoadDatabase" loads the single application DBD file which
includes the EtherIP DBD file.

* EPICS records: Guidelines
The EtherIP driver was designed in order to optimize the tag
transfers. When multiple records are attached to the same tag, the
driver will transfer the tag only once, using the highest scan rate of
the attached records. When records refer to different elements of an
array, the driver will transfer the array as a whole. Tags are
arranged according to PLC and scan rate. In order to not disturb
processing of the EPICS database, the driver has one separate task per
PLC. 

You should try to benefit from the driver optimization by arranging
tags in arrays. You can have alias tags on the PLC, so that a
meaningful alias like "InputFlow" is used in the ladder logic while
the data is also held in an array element "xfer[5]" which the EPICS
record can use for the network transfer.
Arrays should be one-directional: Use separate "EPICS to PLC" and "PLC
to EPICS" arrays. Because of PLC buffer limitations, the array size is
unfortunately limited to about BOOL[352] and REAL[40]. While you can
define bigger arrays, those cannot be transferred over the network
with EtherIP. Consequently you might end up with several transfer arrays.

You should also understand that the network transfer can be delayed or
even fail because of network problems. Consequently you must not
depend on "output" records to write to the PLC within milliseconds. If
e.g. an output on the PLC has to be "on" for a certain amount of time,
have the PLC ladder logic implement this critical timing. The EPICS
record can then write to a "start" tag on the PLC, the PLC handles the
exact timing in response to the command. When done, the PLC signals
success or failure via another "status" tag.
This way, network delays in the transfer of "start" and "status" tags
will not harm the critical timing.
                
* EPICS records: Generic fields
** DTYP: Device type
Has to be "EtherIP" as defined in DBD file:
    field(DTYP, "EtherIP")

** SCAN: Scan Field
The driver has to know how often it should communicate
with the PLC. Per default, it uses the SCAN field
of the record:
    field(SCAN, "1 second")
    field(SCAN, ".1 second")
    field(SCAN, "10 second")
...
The driver scans the PLC at the same rate.
The record simply reads the most recent value.
Note: The scan tasks of the driver and the EPICS database
are not synchronized.

*** Note on multiple records attached to the same tag
When multiple records refer to the same tag, the driver
will scan that tag at the highest scan rate.
Example:
record(ai, "A")
{
    field(INP, "@plc1 fred")
    field(SCAN, "1 second")
    ...
}

record(ai, "B")
{
    field(INP, "@plc1 fred")
    field(SCAN, ".1 second")
    ...
}

-> The driver will scan the tag "fred" at 10 Hz.

This also applies to arrays:
Since requests to array elements my_array[0], my_array[2],
my_array[5],... are combined into a SINGLE transfer of the
tag my_array, the rate of that transfer is the fastes rate
requested for any of the array elements.
(Unless you request single element requests with the 'E'
flag which you should try to avoid).

*** SCAN Passive
Output records are often passive:
They are only processed when the record is accessed via ChannelAccess
from an operator screen where someone entered a new value for this
record.

While the driver will only write to a tag when the record is
processed, it will still try to read the tag from the PLC in case it is
changed from another source (another IOC, PanelView, ...).
The section "Keeping things synchronized" gives details on this.
Since the driver cannot extract an update rate from the SCAN field
when it is set to "Passive", the "S" scan flag has to be used as
described in the INP/OUT link section.

*** SCAN I/O Intr
Input records can be configured to use
    field(SCAN, "I/O Intr")
The driver causes the record to be processed as soon as a new value is
received. As in the Passive case, the driver needs the "S" scan
flag to determine the poll rate.

** INP, OUT: Input/Output Link
The INP field for input records resp. the OUT field for output records
has to match
   field(INP, "@<plc name> <tag> [flags]")
   field(OUT, "@<plc name> <tag> [flags]")

*** <plc name>
    This is the driver's name for the PLC, defined in the vxWorks
    startup script via
       drvEtherIP_define_PLC <name>, <ip_addr>, <slot>
    Example:
       drvEtherIP_define_PLC "plc1", "snsplc1", 0
    More detail on this as well as the IP address mapping
    and routing can be found in the "Installation" section.

*** <tag>
    This can be a single tag "fred" that is defined in the "Controller
    Tags" section of the PLC ladder logic. It can also be an array tag
    "my_array[5]" as well as a structure element "Local:2:I.Ch0Data".
    Array elements are indexed beginning with 0. 
    Note: you can use decimals (2, 10, 15), hex numbers (0x0f) and
    octal numbers (04, 07, 12). This means that 08 is invalid because
    it is interpreted as an octal number!

    The <tag> has to be a single elementary item (scalar tag, array
    element, structure element), not a whole array or structure.

*** <flags>
    There are record-specific flags that will be explained
    later. Common flags are:

    "S <scan period>" - Scan flag
    If the SCAN field does not specify a scan rate as in the case of
    "Passive" output records or input records with SCAN="I/O Intr",
    the S flag has to be used to inform the driver of the requested update
    rate.
    Note that the behavior of the scan flag is only defined for these
    cases:
     Record Type     SCAN
     AI              I/O Intr
     BI		     I/O Intr
     MBBI	     I/O Intr
     MBBIDirect	     I/O Intr
     AO		     Passive
     BO		     Passive
     MBBO	     Passive
     MBBODirect	     Passive

    In all other cases, the S flag should not be used, instead the
    SCAN field must provide the needed period (e.g. SCAN=".5 second").  

    The time format is in seconds, like the SCAN field, but without "seconds".
    Examples:
       field(INP, "@snsioc1 temp S .1")
       field(INP, "@myplc xyz S 0.5")
    There has to be a space after the "S"!

    If the record has neither a periodic SCAN rate nor an S flag in
    the link field, you will get an error message similar to

       devEtherIP (Test_HPRF:Amp_Out:Pwr1_H):
       cannot decode SCAN field, no scan flag given
       Device support will use the default of 1 secs,
       please complete the record config

    In the vxWorks startup file, you can set the double-typed variable
    "drvEtherIP_default_rate" to provide a default rate.
    If you do that, the warning will vanish.
    The recommended practice, however, is to provide a per-record
    "S" flag because then you can recollect the full configuration
    from the record and avoid ambiguities.

    "E" - force elementary transfer
    If the tag refers to an array element,
       field(INP, "@snsioc1 arraytag[5]")
    the driver will combine all array requests into a single array
    transfer for this tag. This is meant to reduce network traffic:
    Records scanning arraytag[0], ... arraytag[5] will result in a single
    "arraytag" transfer for elements 0 to 5.

    The "E" flag overrides this:
       field(INP, "@snsioc1 arraytag[5] E")
    will result into an individual transfer of "arraytag" element 5,
    not combined with other array elements.

    Reasons for doing this:
    a) The software can only transfer array elements 0 to N, always
       beginning at 0. If you need array element 100 and only this element,
       so there is no point reading the array from 0 to 100.
    b) You want array elements 401, 402, ... 410. It's not possible
       for the driver to read 401-410 only, it has to read 0-410. This,
       however, might be impossible because the send/receive buffers of the
       PLC can only hold about 512 bytes. So in this case you have to read
       elements 401-410 one by one with the "E" flag.
    c) Binary record types (bi, bo, mbbi, ...) with a non-BOOL array
       element. See the binary record details below.

    Unless you absolutely have to use the "E" flag for these reasons,
    don't use it.
    It is no problem to have one "BOOL[352]" tag for IOC->PLC
    communication and another "BOOL[352]" array for PLC->IOC
    communication, both at 10Hz. The result is a low and constant
    network load, the transfers are almost predictable even though
    Ethernet is not "deterministic". If instead you use several "E"
    flags, each of those tags ends up being a separate transfer,
    leading to more network load and possible collisions and delays.
  

* ai, Analog Input Record
By default the tag itself is read:

PLC Tag type      Action
------------      ---------------------------------------------------
REAL              VAL field is set (no conversion).
INT, DINT, BOOL   RVAL is set, conversions (linear, ...) can be used.

The analog record cannot be used with BOOL array elements,
other arrays (REAL, INT, ...) are allowed.

** Statistics Flags
The driver holds statistics per Tag which can be accessed with ai
records via the flag field. A valid tag is *always* required. For
e.g. "TAG_TRANSFER_TIME" this makes sense because you query per-tag
information. In other cases it's used to find the scanlist.

    field(INP, "@$(PLC) $(TAG) PLC_ERRORS")
    - # of timeouts/errors in communication with PLC [count]

    field(INP, "@$(PLC) $(TAG) PLC_TASK_SLOW")
    - # times when scan task was slow [count]

    field(INP, "@$(PLC) $(TAG) LIST_TICKS")
    - vx Ticktime when tag's list was checked.
      Useful to monitor that the driver is still running.

    field(INP, "@$(PLC) $(TAG) LIST_SCAN_TIME"),
    field(INP, "@$(PLC) $(TAG) LIST_MIN_SCAN_TIME"),
    field(INP, "@$(PLC) $(TAG) LIST_MAX_SCAN_TIME"),
    - Time for handling scanlist [secs]: last, minumum, maximum

    field(INP, "@$(PLC) $(TAG) TAG_TRANSFER_TIME")
    - Time for last round-trip data request for this tag

The PLC_TASK_SLOW flag is of less use than anticipated. It's
incremented when the scan task is done processing the list and then
notices that it's already time to process the list again. Since all
delays are specified in vxWorks ticks, defaulting to 60 ticks per
second, this scheduling is rather coarse. With all the other task
scheduling going on and ethernet delays, PLC_TASK_SLOW will increment
quite often without a noticeable impact on the data (no time-outs, no
old data).

* ao, Analog Output Record
Like analog input, tags of type REAL, INT, DINT, BOOL are supported as
well as REAL, INT, DINT arrays (no BOOL arrays). No statistics flags
are supported.  

If the SCAN field is "Passive", the "S" flag has to be used.

** Write Caveats

*** Keeping things synchronized
The problem is that the EPICS IOC does not "own" the PLC. Someone else
might write to the PLC's tag (RSLogix, PanelView, another IOC,
command-line program). The PLC can also be rebooted independent from
the IOC. Therefore the write records cannot just write once they have
a new value, they have to reflect the actual value on the PLC.

In order to learn about changes to the PLC from other sources, the
driver scans write tags just like read tags, so it always knows the
current value. When the record is processed, it checks if the value to
be written is different from what the PLC has. If so, it puts its RVAL
into the driver's table and marks it for update
-> the driver writes the new value to the PLC.

So in the case of output records the driver will still read from the PLC
periodically and only switch to write mode once after an output record
has been processed and provided a new value.

Some glue code in the device is called for every value that the driver
gets. It checks if this still matches the record's value. If not, the
record's RVAL is updated and the record is processed. A user interface
tool that looks at the record sees the actual value of the PLC.
The record will not write the value that it just received because
it can see that RVAL matches what the driver has.

This fails if two output records are connected to the same tag,
especially if one is a binary output that tries to write 0 or 1. In
that case the two records each try to write "their" value into the
tag, which is likely to make the value fluctuate.

Another side effect is that when processing an output record,
that record will not write immediately. The writing is handled
by a separate thread in the driver. The next time the tag is scanned,
the driver thread will notice the "update" flag and write to the PLC.
Consequently you adjust the write latency when you specify the scan
rate of the driver thread.

*** Dropped messages
The EtherIP driver uses TCP. So unless the TCP layer reports
errors, all messages to the PLC should eventually reach the PLC.
And unless the PLC reports an error in response to a write request,
the write request should actually change the affected PLC tag.

In practice, Herb Strong and Pam Gurd (ORNL) noticed that
some write requests do not reach the PLC.
It is unclear, when this starts to happen
(certain number of network packages per second?).
The cause seems to be between the ControlLogix ENET module
and the ControlLogix Controller. Both use ControlNet
to communication via the ControlLogix backplane.
Though TCP shows no errors and ControlNet is supposed to
be reliable and deterministic, some write requests
get dropped on the way from ENET to the controller.

There is no error message for the driver to notice this,
and no known threshold that causes this to happen.

*** "FORCE" Flag
Whenever an output record is processed, it will
update the driver's copy of a tag and mark it for "write".
The next time the driver processes the scan list which
contains the tag, it will write the tag to the PLC.

When the record is not processed, and therefore the tag
is not marked for write, the driver will read the
tag from the PLC.
What happens when the value of the tag differs from
the value of the record?

Per default, the record is updated to reflect the value of the
tag. This way, both the IOC and e.g. a PanelView display can change
the same PLC tag. Changes from "one" source are reflected on the
respective "other" side.
With TPRO, it looks like this:
     'Test_HPRF:Fil1:WrmRmp_Set': got 8 from driver
     'Test_HPRF:Fil1:WrmRmp_Set': updated record's value 8  

The "FORCE" flag will change this behavior:
When the driver notices a discrepency, it will NOT
change the record but simply re-process it.
This causes the IOC to write to the tag on the PLC
again and again until the tag on the PLC matches
the value of the record. The record tries to "force"
its value into the tag.
With TPRO, it looks like this:
     'Test_HPRF:Xmtr1:FilOff_Cmd': got 0 from driver
     'Test_HPRF:Xmtr1:FilOff_Cmd': will re-write record's value 1

*** Arrays
When writing array tags, a single ao record (or bo, mbbo, ...)
is connected to a single element of the array.
When the record has a new value, it will update that array
element and mark the array as "please write to PLC during the
next scan cycle of the driver".
This is desirable because it allows several output records to
specify new values and then the WHOLE ARRAY is written as one unit.

Writing the values that didn't change doesn't matter because
a) the transfer time for a single tag and an array is almost
   the same. Transferring an array where many items didn't change
   is not costly, transferring two separate tags that did change
   would take longer.
b) the PLC doesn't care if tags are written. There is no
   "tag was written" event in the PLC that I know of.
   Writing the same value again does not upset the ladder logic.

Possible problem:
DO NOT MIX DIRECTIONS within an array.
Do use arrays instead of single tags to speed up the transfer,
but keep different "EPICS to PLC" and "PLC to EPICS" arrays.
If you have to have handshake tags (EPICS writes, PLC uses
it and then PLC resets the tag), those bidirectional tags
should not be in arrays. They have to be standalone, scalar tags.
   
* bi, Binary Input Record
Reads a single bit from a tag.

PLC Tag type      Action
------------      ---------------------------------------------------
BOOL              VAL field is set to the BOOL value
other             converted into UDINT, then bit 0 is read

BOOL Arrays can be used:
   field(INP, "@plc1 BOOLs[52]")
will read the 52nd element of the BOOL array.

INT, DINT arrays are treated as bit arrays:
   field(INP, "@plc1 DINTs[40]")
will *NOT* read array element #40 but bit #40 which is bit # 8 in the
second DINT.

If you want to read the first bit of DINT #40, the "E" flag can be
used to make an elementary request for "DINTs[40]". The preferred solution,
though, is the Bit flag.
The TPRO field (see the section on debugging) is often helpful in
analyzing what array element and what bit is used.

** "B <bit>": Bit flag
   field(INP, "@plc1 DINTs[1] B 8")
will read bit #8 in the second DINT array element.

** write caveats
See the ao comments.

* mbbi, mbbiDirect Multi-bit Binary Input Records
These records read multiple consecutive bits, the count is given in
the number-of-bits field:
   field(NOBT, "3")

The input specification follows the bi description,
except that the addressed bit is the first bit.

When using array elements, the same bit-addressing applies. As a
result, the "B <blit>" flag should be used for non-BOOL arrays.

Note: In the current implementation, the mbbiX records can read across array
elements of DINT arrays. This record reads element 4, bit 31 and
element 5, bit 1:
        field(INP, "@$(PLC) DINTs[4] B 31")
        field(NOBT, "2")
But this feature is merely a side effect, it's safer to read
within one INT/DINT. Or use BOOL arrays.

* bo, mbbo, mbboDirect Binary Output Records
The output records use the same OUT configurations as the
corresponding input records.

If the SCAN field is "Passive", the "S" flag has to be used.

Note that if several records read and write different elements of an
array tag X, that tag is read once per cycle from element 0 up to the
highest element index N that any record refers to. If any output record
modifies an entry, the driver will write the array (0..N) in the next
cycle since it is marked as changed.

As a result, it is advisable to keep "read" and "write" arrays
separate, because otherwise elements meant for "read" will be written
whenever one or more other elements are changed by output records.

** write caveats
See the ao comments.

* Debugging
The driver can display information via the usual EPICS dbior call
on the IOC vxWorks console (or a telnet connection to the IOC):
    dbior "drvEtherIP", 10
A direct call to
    drvEtherIP_report 10
yields the same result. Instead of 10, lower levels of verbosity are
allowed.

Hint: It's useful to redirect the output to the host:
    drvEtherIP_report 10 >/tmp/eip.txt
Then, on the Win32 or Unix host, open that file
with EMACS. The outline format allows easy browsing.

drvEtherIP_help shows all user-callable driver routines:
    -> drvEtherIP_help
    drvEtherIP V1.1 diagnostics routines:
      int EIP_verbosity:
      -  set to 0..10
      double drvEtherIP_default_rate = <seconds>
       -  define the default scan rate
          (if neither SCAN nor INP/OUT provide one)    
      drvEtherIP_define_PLC <name>, <ip_addr>, <slot>
      -  define a PLC name (used by EPICS records) as IP
         (DNS name or dot-notation) and slot (0...)
      drvEtherIP_read_tag <ip>, <tag>, <elements>, <ms_timeout>
      -  call to test a round-trip single tag read
      drvEtherIP_report <level>
      -  level = 0..10
      drvEtherIP_dump
      -  dump all tags and values; short version of drvEtherIP_report
      drvEtherIP_reset_statistics
      -  reset error counts and min/max scan times
      drvEtherIP_restart
      -  in case of communication errors, driver will restart,
         so calling this one directly shouldn't be necessary
         but is possible                           

A common problem might be that a record does not seem to read/write
the PLC tag that it was supposed to be connected to.
When setting "TPRO" for a record, EPICS will log a message whenever a
record is processed. The EtherIP device support shows some additional
info on how it interpreted the INP/OUT link. Use a display manager, a
command line channel access tool or
    dbpf "record.TPRO", "1"
in the vxWorks shell to set TPRO. Set TPRO to "0" to switch this off again.

Example output for a binary input that addresses "DINTs[40]":
process:   snsioc4:biDINTs40
   link_text  : 'plc1 DINTs[40]'
   PLC_name   : 'plc1'
   string_tag : 'DINTs'
   element    : 1          <- element 1!
   mask       : 0x100      <- mask selects bit 8!

As you see, the BI record is reading bit #8
in DINT[1], that's bit #40 when counting from the
beginning of the DINT array.
If that's what you wanted, OK.
If you entered "DINTs[40]" because you wanted bit #0
in array element 40, you should have used "DINTs[40] B 0"
(See the description of the bi record and the "B" flag)

** Checklist
( ) Set the record's TPRO to "1".
    Does the record get processed when you want it to be processed?
    Does the link_text make sense?
    Is it parsed correctly, i.e. is the PLC_name what you
    meant to use for a PLC name?
    Does the combination of string_tag, element & mask make sense?
( ) All "drvEtherIP_report 10", locate the information
    for the tag that the record uses:
    *** Tag 'Word2' @ 0x18F0F38:
      scanlist            : 0x191B5F0
      compiled tag        : 'Word2'
      elements            : 29
      cip_r_request_size  : 12
      cip_r_response_size : 64
      cip_w_request_size  : 72
      cip_w_response_size : 4
      data_lock ID        : 0x18F0ED8
      data_size (buffer)  : 60
      valid_data_size     : 60
      do_write            : no
      is_writing          : no
      data                : INT 4 1 1 1 4 0 0 0 0 0 1 1 1 1\
                           4 1 1 1 1 1 2 2 1 16 0 0 4 1 1 
      transfer tick-time  : 46 (0.046 secs) 
    If the "...._size" fields in there are zero, the driver
    could not learn anything about the tag.
    See if the tag actually exists on the PLC (next step).
    Note on arrays:
    Requests are combined. Assume that we are debugging a record
    that accesses tag FRED[7]. drvEtherIP_report might show
    that the driver is actually trying to access 10 elements
    for tag FRED. That means that some other record must
    try to get FRED[9], so alltogether the driver reaches
    for FRED[0]...FRED[9] -> 10 elements.
    Assert that there are at least 10 elements for the tag FRED
    on the PLC!
( ) Use the ether_ip_test tool, e.g. try
       ether_ip_test -i 123.45.67 MyTag[12]
    to see if you can get to the PLC and read the tag.

* EIP_verbosity
Depending on the value of EIP_verbosity, the driver
and device support will report various levels of detail:

 0: Only severe errors are reported
 1: Show errors and some more information
...
 9: Show a hex-dump of all the network traffic
    that's sent and received
10: Show byte-by-byte explanation of what's assembled
    in the send buffer, hex-dump of that buffer,
    hex-dump of the received data and byte-by-byte
    explanation of what has been received.

A LOT of output is generated at levels 9 & 10, resulting
in a significant CPU load and - when viewed in a telnet
session - network traffic. Therefore all output can be
redirected into a memory string file (ring buffer, see mem_string_file.h):

    # Redirect EIP diagnostics output to ring buffer,
    # enable full diag. messages:
    EIP_use_mem_string_file=1
    EIP_verbosity=10

The idea is to keep writing into the ring buffer until
you believe you caught what you were after. Now
stop recording (and overwriting) by setting EIP_verbosity=0.
To view (& clear) the current buffer on stdout:
    msfDump(0)
Copy the current buffer into file (& clear the buffer):
    f=fopen("/tmp/eip.log", "w")
    msfDump(f)
    fclose(f)      

The size of the memory string file defaults to 2MB.
It can be changed in the vxWorks startup file:
    msfInitialBufferSize=10*1024*1024

* Driver Operation Details
Example:
Records
   "fred", 10 seconds
   "freddy", 10 seconds
   "jane", 10 seconds
   "analogs.temp[2].input", 5 seconds
   "binaries[3] E", 1 Hz
   "binaries", element 1, 10Hz
   "binaries", element 5, 10Hz
   "binaries", element 10, 10Hz

Scanlist created from this
   10  Hz: "binaries", elements 0-10
    1  Hz: "binaries[3]"
    0.5Hz: "analogs.temp[2].input"
    0.1Hz: "fred", "freddy", "jane"

Driver actions
   One thread and socket per PLC for communication.
   One TagInfo per tag: name, elements, sizes.
   ScanTask: runs over scanlists for its PLC.
   For each scanlist:
       Figure out how many requests can be combined
       into one request/response round-trip
       (~500 byte limit), record in TagInfo.

* CIP data details
Analog array REALs[40]:
Read "REALS",    2 elements -> REAL[0], REAL[1]

Binary array BOOLs[352]:
Read "BOOLs",     1 element  -> 32bit DINT with bits  0..31

Access to binaries is translated inside the driver.
Assume access to "fred[5]".

For analog records, a request to the 5th array element is assumed.
For binary records, we assume that the 5th _bit_ should be addressed.
Therefore the first element (bits 0-31) is read and the single
bit in there returned.

* Files
ether_ip.[ch]    EtherNet/IP protocol
dl_list*         Double-linked list, used by the following
drvEtherIP*      vxWorks driver
devEtherIP*      EPICS device support
ether_ip_test.c  main for Unix/Win32
