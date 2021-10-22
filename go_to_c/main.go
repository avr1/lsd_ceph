package main

// #cgo CFLAGS: -g -Wall
// #include <stdlib.h>
// #include "f_to_c.h"
import "C"

import "fmt"

func main() {
	fmt.Print("32 degrees Fahrenheit is:")
	c := C.f_to_c(32)
	fmt.Println(c)
}
