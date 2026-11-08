extern putchar(c: u8) -> void;

MAX_BUFFER :: 64;

print :: (s: []u8) -> void {
    for c: s {
        putchar(c);
    }
    putchar(10);
}

#method(*i32)
increment :: (ptr: *i32, amount: i32) -> void {
    *ptr = *ptr + amount;
}

#param_constraints(count <= MAX_BUFFER)
fill :: (buffer: *[u8; MAX_BUFFER], count: i32, value: u8) -> void {
    for i: 0..<count {
        (*buffer)[i] = value;
    }
}

main :: () -> i32 {
    value: i32 = -5;
    ptr: *i32 = &value;

    (ptr).increment(5);

    buffer: [u8; MAX_BUFFER];

    fill(&buffer, 20, 'A');
    print(buffer[0..<20]);

    return value;
}
