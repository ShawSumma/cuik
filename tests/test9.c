
// These are the fun extensions :)
struct Foo {
	int a, b;
};

int bar(struct Foo* f) {
	return f.a + foo() + f.b;
}

int foo() {
	return 16;
}
