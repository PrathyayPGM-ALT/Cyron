# Classes, inheritance, and super.
class Shape {
    fn init(name) {
        this.name = name
    }
    fn area() { return 0 }
    fn describe() {
        print(this.name, "has area", this.area())
    }
}

class Circle : Shape {
    fn init(r) {
        super.init("circle")
        this.r = r
    }
    fn area() {
        return 3.14159265 * this.r * this.r
    }
}

class Rect : Shape {
    fn init(w, h) {
        super.init("rect")
        this.w = w
        this.h = h
    }
    fn area() { return this.w * this.h }
}

let shapes = [Circle(2), Rect(3, 4), Circle(1)]
for s in shapes {
    s.describe()
}
