EXTERNAL_OPTIONS=
DEBUG?=0
JOBS?=8

all: hooks src

external:
	# cd external ; make DEBUG=$(DEBUG) JOBS=$(JOBS) $(EXTERNAL_OPTIONS);

src: external
	cd src ; make ; 

src-fast: external
	cd src ; make core-fast DEBUG=$(DEBUG) JOBS=$(JOBS);
	cd src ; make tools-fast DEBUG=$(DEBUG) JOBS=$(JOBS);

tests: src
	cd tests ; make ;

hooks:
	make -C .githooks

format:
	cd src ; ./scripts/format_source_code.sh

clean:
	cd external ; make clean ; 
	cd src ; make clean ; 
	cd tests ; make clean; 
	cd examples ; make clean ; 
	find ./ -name .clangd -exec rm -rv {} +
	find ./ -name .cache -exec rm -rv {} +

uninstall: clean
	rm -f enable ;
	rm -rf install ;
	cd external ; make $@
	if test -d .githooks ; then cd .githooks ; make clean ; fi;

.PHONY: src src-fast tests hooks format clean uninstall external
