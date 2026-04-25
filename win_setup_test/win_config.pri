# Windows Environment Configuration for DMX_qt Project
# Include this file in your .pro using: include(win_setup_test/win_config.pri)

win32 {
    PROJECT_ROOT = $$clean_path($$PWD/..)
    # =========================================================
    # 1. Qt Configuration (Qt 5.4 MinGW 32-bit)
    #    Location: E:/qt5.4/5.4/mingw491_32/
    # =========================================================
    QT_ROOT = E:/qt5.4/5.4/mingw491_32

    # =========================================================
    # 2. OpenCV Configuration
    #    Version: 3.4.16
    #    Location: E:/opencv3.4/opencv-3.4.16
    # =========================================================
    OPENCV_ROOT = E:/opencv3.4/opencv-3.4.16
    OPENCV_INSTALL = E:/opencv3.4/install_mingw491_32

    INCLUDEPATH += $$OPENCV_INSTALL/include

    OPENCV_LIBDIR =
    exists($$OPENCV_INSTALL/x64/mingw/lib): OPENCV_LIBDIR = $$OPENCV_INSTALL/x64/mingw/lib
    exists($$OPENCV_INSTALL/x86/mingw/lib): OPENCV_LIBDIR = $$OPENCV_INSTALL/x86/mingw/lib
    else: exists($$OPENCV_INSTALL/lib): OPENCV_LIBDIR = $$OPENCV_INSTALL/lib
    else: exists($$OPENCV_INSTALL/x86/mingw/staticlib): OPENCV_LIBDIR = $$OPENCV_INSTALL/x86/mingw/staticlib

    !isEmpty(OPENCV_LIBDIR) {
        LIBS += -L$$OPENCV_LIBDIR

        exists($$OPENCV_LIBDIR/libopencv_world3416.dll.a): LIBS += -lopencv_world3416
        else: exists($$OPENCV_LIBDIR/libopencv_world3416.a): LIBS += -lopencv_world3416
        else: LIBS += -lopencv_core3416 -lopencv_imgproc3416 -lopencv_imgcodecs3416 -lopencv_videoio3416 -lopencv_highgui3416
    }

    # =========================================================
    # 3. FFmpeg Configuration
    #    Location: win_setup_test/3rdparty/ffmpeg/
    # =========================================================
    FFMPEG_ENABLE = 1
    FFMPEG_PATH = $$PROJECT_ROOT/win_setup_test/3rdparty/ffmpeg_mingw
    INCLUDEPATH += $$FFMPEG_PATH/include

    equals(FFMPEG_ENABLE, 1) {
        exists($$FFMPEG_PATH/lib) {
            exists($$FFMPEG_PATH/lib/libavcodec.dll.a) {
                LIBS += -L$$FFMPEG_PATH/lib -lavcodec -lavformat -lavutil -lswscale -lavdevice -lswresample
            } else: exists($$FFMPEG_PATH/lib/avcodec.lib) {
                LIBS += $$FFMPEG_PATH/lib/avcodec.lib \
                        $$FFMPEG_PATH/lib/avformat.lib \
                        $$FFMPEG_PATH/lib/avutil.lib \
                        $$FFMPEG_PATH/lib/swscale.lib \
                        $$FFMPEG_PATH/lib/avdevice.lib \
                        $$FFMPEG_PATH/lib/swresample.lib
            }
        }
    }

    # =========================================================
    # 4. Runtime DLL Path
    # =========================================================
    equals(FFMPEG_ENABLE, 1): exists($$FFMPEG_PATH/bin): LIBS += -L$$FFMPEG_PATH/bin

    # =========================================================
    # 5. Qt Modules Required
    # =========================================================
    QT += core gui multimedia multimediawidgets serialport network

    greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

    CONFIG += c++11

    # =========================================================
    # 6. Compiler Settings
    # =========================================================
    # Use Qt MinGW kit: qmake -spec win32-g++ && mingw32-make
}
