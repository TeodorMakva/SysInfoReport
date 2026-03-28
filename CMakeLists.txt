cmake_minimum_required(VERSION 4.0)
project(SysInfoReport)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC_COMMON
        main.cpp
        ReportSection.h
        ReportFormatter.h
        ReportFormatter.cpp
        OsSectionProvider.h
        CPUSectionProvider.h
        GPUSectionProvider.h
        DiskSectionProvider.h
        MotherboardSectionProvider.h
        MemorySectionProvider.h
        NetworkSectionProvider.h
)

if (WIN32)
    add_executable(SysInfoReport
            ${SRC_COMMON}
            wide_to_utf8.h
            OsSectionProviderWin.cpp
            CPUSectionProviderWin.cpp
            GPUSectionProviderWin.cpp
            DiskSectionProviderWin.cpp
            MotherboardSectionProviderWin.cpp
            MemorySectionProviderWin.cpp
            NetworkSectionProviderWin.cpp
    )

    target_link_libraries(SysInfoReport
            PRIVATE
            dxgi
            psapi
            wbemuuid
            iphlpapi
            ws2_32
    )
else()
    add_executable(SysInfoReport
            ${SRC_COMMON}
            LinUtils.h
            OsSectionProviderLin.cpp
            CPUSectionProviderLin.cpp
            GPUSectionProviderLin.cpp
            DiskSectionProviderLin.cpp
            MotherboardSectionProviderLin.cpp
            MemorySectionProviderLin.cpp
            NetworkSectionProviderLin.cpp
    )

    # если понадобится что‑то типа libpci / libudev / libnl, добавишь здесь
    # target_link_libraries(SysInfoReport PRIVATE pci udev ...)
endif()