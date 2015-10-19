# This depends on rst2html and graphviz, and the resulting html uses
# http://wavedrom.com/ online for rendering the timeline. The offline wavedrom
# conversion seems a bit tricky to install, but is possible if needed. To edit
# the wavedrom json, copy-pasting to and from http://wavedrom.com/editor.html is
# handy as it shows the result live.

all: drm-intel.html dim.html

%.svg: %.dot
	dot -T svg -o $@ $<

%.html: %.rst
	rst2html $< > $@

# the sed bit here is a hack to make wavedrom process the timeline
drm-intel.html: drm-intel.rst drm-intel-flow.svg drm-intel-timeline.rst drm-intel-timeline.json
	rst2html $< > $@
	sed -i 's/<body/<body onload="WaveDrom.ProcessAll()"/' $@

dim.html: dim.rst

clean:
	rm -f drm-intel.html drm-intel-flow.svg dim.html

.PHONY: all clean
