# Functions are values and capture their environment.
fn make_counter() {
    let count = 0
    fn tick() {
        count += 1
        return count
    }
    return tick
}

let a = make_counter()
let b = make_counter()
a()
a()
print("a:", a(), " b:", b())   # a: 3  b: 1

fn twice(f, x) { return f(f(x)) }
fn double(n) { return n * 2 }
print("twice double 5 =", twice(double, 5))
