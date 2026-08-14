# Rei programming language

this language is currently just for fun.

[syntax](./SYNTAX.bnf)

## How to build

only tested on linux

### Dependencies

- gcc
- fasm
- ar

```sh
gcc nob.c -o nob
./nob
./nob run ./examples/hello.rei
./hello
```

## References

- [nob.h](https://github.com/tsoding/nob.h) - for build recipe
- [ht.h](https://github.com/tsoding/ht.h) - for hash tables
- [Jai](https://jaiprogramming.com/) - as an inspiration
- [B Lang](https://github.com/bext-lang/b/) - compiler inspiration & rule110 example

## License

[MIT or Public Domain](./LICENSE)
