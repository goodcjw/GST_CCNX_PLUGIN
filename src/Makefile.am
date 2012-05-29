# Note: plugindir is set in configure

plugin_LTLIBRARIES = libgstccnx.la

# sources used to compile this plug-in
libgstccnx_la_SOURCES = \
	gstCCNxSrc.c \
	gstCCNxSrc.h \
	gstCCNxDepacketizer.c \
	gstCCNxDepacketizer.h \
	gstCCNxSegmenter.c \
	gstCCNxSegmenter.h \
	gstCCNxFetchBuffer.c \
	gstCCNxFetchBuffer.h \
	gstCCNxUtils.c \
	gstCCNxUtils.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstccnx_la_CFLAGS = $(GST_CFLAGS)
libgstccnx_la_LIBADD = $(GST_LIBS) -lccn -lcrypto
libgstccnx_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstccnx_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = \
	gstCCNxSrc.h \
	gstCCNxDepacketizer.h \
	gstCCNxSegmenter.h \
	gstCCNxUtils.h
