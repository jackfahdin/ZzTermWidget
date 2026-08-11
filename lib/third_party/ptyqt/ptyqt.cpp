#include "ptyqt.h"
#include <utility>

#ifdef Q_OS_WIN

// Windows uses the native ConPty backend (Windows 10 1903+) for both MSVC and MinGW.
#include "conptyprocess.h"

IPtyProcess *PtyQt::createPtyProcess(IPtyProcess::PtyType ptyType) {
    Q_UNUSED(ptyType);
    return new ConPtyProcess();
}

#endif

#ifdef Q_OS_UNIX

#include "unixptyprocess.h"

IPtyProcess *PtyQt::createPtyProcess(IPtyProcess::PtyType ptyType) {
    Q_UNUSED(ptyType);
    return new UnixPtyProcess();
}

#endif
