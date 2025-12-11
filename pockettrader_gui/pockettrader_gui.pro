QT       += widgets
CONFIG   += c++11

TEMPLATE = app
TARGET   = pockettrader_gui

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

# path to pockettrader_state.h on the BBB
INCLUDEPATH += ../pockettrader_core_user_space

# we use pthread + shm
LIBS += -pthread -lrt
