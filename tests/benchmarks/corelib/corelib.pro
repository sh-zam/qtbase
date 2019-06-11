TEMPLATE = subdirs
SUBDIRS = \
        io \
        json \
        mimetypes \
        kernel \
        thread \
        time \
        tools \
        codecs \
        plugin

TRUSTED_BENCHMARKS += \
    kernel/qmetaobject \
    kernel/qmetatype \
    kernel/qobject \
    thread/qthreadstorage \
    io/qdir/tree

include(../trusted-benchmarks.pri)
