# go-to-c spike

This is a super barebones, 1-hour timeboxed way o integrate calls between C and Go in a manner that
might be useful for exposing our C-compatible API with rbd. 

Here, I've written a header file and .c file for a function that converts a temperature in
fahrenheit to celsius. I've also written a main.go file that drives the code, feeding in 32 as an
argument.

Things to learn from this:
    - It is possible to write functions that are written in C in Go, but it is not good for types.
      For example, a limitation with this program is that I can't get input from the user in Go and
      pass it to C, because the languages inherently store data differently, based on the
      implementation and the hardware. 
    - However, it is easy to go both ways, from C to Go, and Go to C. I'm not certain how much this
      will benefit us, as we are looking to change the research device (already in Go), but it might
      be of some use.

Resources I found:

[Tutorial](https://karthikkaranth.me/blog/calling-c-code-from-go/)
[Official Documentation](https://pkg.go.dev/cmd/cgo@go1.17.1)
