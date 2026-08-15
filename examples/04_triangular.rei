#printf_like(0)
extern printf(*u8, ...) -> i32;

/* recursive triangular numbers function
 * im aware this is not the most efficient way to calculate triangular numbers,
 * but it is a good example of recursion and default parameters */
t :: (i: i32, j: i32 = 0) -> i32 {
    if (j == i) {
        return j;
    }
    return t(i, j + 1) + j;
}

main :: () -> i32 {
    for i: 0..25 {
        printf(c"T(%2d) = %d\n", i, t(i));
    }
    return 0;
}
