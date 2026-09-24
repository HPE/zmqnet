#
# Makefile.am for HTML documentation.
#
# Copyright 2023 Hewlett Packard Enterprise Development LP.
#
# Unpublished Proprietary Information.
# This unpublished work is protected to trade secret, copyright and other laws.
# Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
# no part of this work or its content may be used, reproduced or disclosed
# in any form.

htmlbuilddir = sphinx/_build/html
htmldestdir = $(datadir)/doc

man3_MANS = zmqnet.3

MARKDOWN_IN = sphinx/description.md sphinx/environment.md sphinx/cti.md sphinx/topology.md \
	sphinx/filters.md sphinx/streams_and_communicators.md sphinx/frontend.md sphinx/backend.md \
	sphinx/examples/capnp.md sphinx/see_also.md

all: $(MARKDOWN_IN)
	./sphinx_build.sh -b html -d $(htmlbuilddir)/doctrees sphinx/ $(htmlbuilddir)

install-exec-hook:
	mkdir -p $(htmldestdir)/_static
	cp $(htmlbuilddir)/*.html $(htmldestdir)
	cp $(htmlbuilddir)/*.js $(htmldestdir)
	cp -r $(htmlbuilddir)/_static/* $(htmldestdir)/_static

zmqnet.3.md: manpage_header.md $(MARKDOWN_IN)
	cat manpage_header.md $(MARKDOWN_IN) > zmqnet.3.md

zmqnet.3: zmqnet.3.md
	./generate_man.sh zmqnet.3.md

