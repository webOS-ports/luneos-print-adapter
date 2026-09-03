luneos-print-adapter
====================

Summary
-------
Printing over CUPS and IPP on the luna-service2 bus as `com.palm.printmgr`.

Description
-----------
The webOS printing API, reimplemented on top of CUPS and IPP Everywhere.

The service registers as **`com.palm.printmgr`** (and `com.webos.service.print`)
and serves the original two categories and 23 methods, so the existing enyo
print dialog and the replicated Print Manager UI work unchanged. Underneath it
is CUPS rather than HP's `wprint` PDL plugins, which were closed-source ARM-only
blobs and the reason the original stack really only worked with HP hardware.

    apps / print dialog  ──LS2──▶  luneos-print-adapter  ──▶  CUPS  ──IPP──▶  printer
                                                               ▲
                                                          avahi (mDNS)

What it deliberately does not do
--------------------------------
No PDL generation, no PCL, no raster pipeline, no SNMP, no HP LEDM polling. Any
printer worth supporting since roughly 2013 speaks IPP Everywhere / AirPrint and
accepts PDF or JPEG directly, so the whole rendering layer the original carried
is gone. CUPS handles discovery, the filter chain, queueing, retry and job
state; this daemon translates.

API
---
Unchanged from webOS 3.0.5:

    /printers   list getCapabilities getCurrent setCurrent
                add delete listAddedPrinters listActive

    /jobs       open getCurrentPrintParams editPrintParams
                getFinalParamsAndArea getAreaForInsets
                newTempFile resizeTempFile addFile close cancel
                getStatus getRenderStatus setRenderStatus
                allAtOnce listAll

The keyword vocabulary (`US_Letter`, `Photo Glossy`, `Best`, `Book`, `Tray1`, …)
and the numeric error codes (`-610` bad syntax, `-617` unknown job, …) are
preserved exactly; clients switch on both.

The surface was recovered from the shipped webOS 3.0.5 `printmgrd` binary, which
is not stripped and embeds a JSON schema per method as string constants, then
confirmed against a live TouchPad driven through its built-in virtual printer.

Source layout
-------------
    src/param_map.*      webOS <-> IPP keyword translation. No deps, unit tested.
    src/print_errors.*   The original error codes and their exact texts.
    src/job_manager.*    Job lifecycle, spool files, printable-area geometry.
    src/printer_db.*     Manually added printers, SSID-scoped, GKeyFile backed.
    src/cups_backend.*   Everything that touches libcups.
    src/print_service.*  LS2 registration and the 23 method handlers.

Tests
-----
    ./tests/run-tests.sh

Covers the mapping tables and the printable-area arithmetic. The area maths is
checked against pixel dimensions captured from a real TouchPad running the
original daemon, so a regression there shows up as a failing test rather than as
clipped output on paper.

The rest needs luna-service2, pbnjson and cups, and only builds in the Yocto
sysroot.

Deliberate differences from the original
----------------------------------------
* **`getFinalParamsAndArea` emits valid JSON.** The original response is
  malformed — it ends `"renderInReverseOrder":false,}` with a trailing comma
  before the closing brace. That is a bug, not a contract.
* **`allAtOnce` is a wrapper, not a separate path.** On the original it could
  return `returnValue:true` and then silently drop the job, producing no page
  and no error (observed on device). Here it runs the same job machinery as the
  long-hand path, so a failure is always a real error code.
* **No file cache.** `newTempFile`/`resizeTempFile` are backed by a per-job
  spool directory instead of `com.palm.filecache`. The `-233` out-of-room error
  is kept for the disk-full case.

Runtime dependencies
--------------------
`cupsd` must be running: it owns the queues and does the mDNS browsing. Avahi
provides the discovery, and oe-core builds CUPS with `--with-dnssd=avahi`
whenever `zeroconf` is in `DISTRO_FEATURES`, which LuneOS sets.

`cups-filters` is recommended rather than required. Many AirPrint printers
accept only `image/urf` or `image/pwg-raster`, and converting PDF to those is
what cups-filters does; without it those printers fail at the filter stage. It
costs poppler, qpdf, lcms, tiff and ghostscript, so it can be dropped with
`BAD_RECOMMENDATIONS` where image size matters more than printer coverage.
