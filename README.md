# VEIKK Driver for Linux

### Build
TODO: prerequisites

```bash
$ make [target]
```
make targets:
- build (default): regular compile
- debug: compile with extra debugging output (in kernel logs)
- clean: clean compiled files
- install: install kernel module
- uninstall: uninstall kernel module

Sample workflows:
```bash
$ # build and install
$ make && sudo make install
$ # uninstall
$ sudo make uninstall
$ # clean everything and rebuild w/ debug
$ make clean debug && sudo make uninstall install
```