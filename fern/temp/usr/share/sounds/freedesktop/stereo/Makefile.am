NULL =

sounddir = $(datadir)/sounds/freedesktop/stereo

sound_DATA =                            \
	alarm-clock-elapsed.oga	        \
	audio-channel-front-center.oga	\
	audio-channel-front-left.oga	\
	audio-channel-front-right.oga	\
	audio-channel-rear-center.oga	\
	audio-channel-rear-left.oga	\
	audio-channel-rear-right.oga	\
	audio-channel-side-left.oga	\
	audio-channel-side-right.oga	\
	audio-test-signal.oga		\
	audio-volume-change.oga		\
	bell.oga			\
	camera-shutter.oga		\
	complete.oga			\
	device-added.oga		\
	device-removed.oga		\
	dialog-information.oga		\
	dialog-warning.oga		\
	message.oga			\
	message-new-instant.oga		\
	phone-incoming-call.oga		\
	phone-outgoing-busy.oga		\
	phone-outgoing-calling.oga      \
	service-login.oga		\
	service-logout.oga		\
	suspend-error.oga		\
	trash-empty.oga			\
	$(NULL)

install-data-hook:
	-rm -f $(DESTDIR)$(sounddir)/power-plug.oga
	$(LN_S) device-added.oga $(DESTDIR)$(sounddir)/power-plug.oga
	-rm -f $(DESTDIR)$(sounddir)/power-unplug.oga
	$(LN_S) device-removed.oga $(DESTDIR)$(sounddir)/power-unplug.oga
	-rm -f $(DESTDIR)$(sounddir)/network-connectivity-established.oga
	$(LN_S) device-added.oga $(DESTDIR)$(sounddir)/network-connectivity-established.oga
	-rm -f $(DESTDIR)$(sounddir)/network-connectivity-lost.oga
	$(LN_S) device-removed.oga $(DESTDIR)$(sounddir)/network-connectivity-lost.oga
	-rm -f $(DESTDIR)$(sounddir)/screen-capture.oga
	$(LN_S) camera-shutter.oga $(DESTDIR)$(sounddir)/screen-capture.oga
	-rm -f $(DESTDIR)$(sounddir)/dialog-error.oga
	$(LN_S) dialog-warning.oga $(DESTDIR)$(sounddir)/dialog-error.oga
	-rm -f $(DESTDIR)$(sounddir)/window-attention.oga
	$(LN_S) dialog-warning.oga $(DESTDIR)$(sounddir)/window-attention.oga
	-rm -f $(DESTDIR)$(sounddir)/window-question.oga
	$(LN_S) dialog-warning.oga $(DESTDIR)$(sounddir)/window-question.oga

EXTRA_DIST =            \
	$(sound_DATA)	\
	$(NULL)

DISTCLEANFILES = \
	$(NULL)

MAINTAINERCLEANFILES =		\
	*~			\
	Makefile.in		\
	$(NULL)
