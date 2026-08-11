extern putchar(c: i32) -> i32;
extern printf(fmt: *u8, ...) -> i32;

/* custom puts for fat ptr strings */
puts :: (s: []u8) -> i32 {
    for c: s { /* example of iterating over a fat ptr string */
        putchar(c as i32);
    }
    putchar(10);
    return 0;
}

main :: (argv: [][]u8) -> i32 {
    for i: 0..<argv.len { /* example of iterating over a range */
        printf(c"%d: ", i);
        puts(argv[i]);
    }
}
