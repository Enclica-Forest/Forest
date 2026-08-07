# build/features/opengl.mk
#
# OpenGL software renderer gating. GL_SRCS and GL_OBJECTS are defined in
# kernel-sources.mk. This fragment empties them when ENABLE_OPENGL=no so
# the GL sources are excluded from the build.

ifeq ($(ENABLE_OPENGL),yes)
COMMON_CFLAGS += -DENABLE_OPENGL
else
# Override GL_SRCS and GL_OBJECTS to empty when disabled.
GL_SRCS    :=
GL_OBJECTS :=
endif
