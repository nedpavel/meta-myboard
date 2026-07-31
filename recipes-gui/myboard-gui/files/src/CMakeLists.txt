cmake_minimum_required(VERSION 3.16)

project(myboard-gui VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt automatisch: MOC für Q_OBJECT, RCC, UIC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt6 Widgets finden
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(myboard-gui
    main.cpp
    mainwindow.cpp
    mainwindow.h
    canmatrix.cpp
    canmatrix.h
    canreader.cpp
    canreader.h
    canopen.cpp
    canopen.h
    images.qrc
    can.qrc
)

target_link_libraries(myboard-gui PRIVATE Qt6::Widgets)

# Installationsziel — landet in /usr/bin
install(TARGETS myboard-gui
    RUNTIME DESTINATION bin
)
