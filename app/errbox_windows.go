package main

import (
	"syscall"
	"unsafe"
)

// showError puts up a message box: the Windows build has no console window to print to.
func showError(msg string) {
	user32 := syscall.NewLazyDLL("user32.dll")
	box := user32.NewProc("MessageBoxW")
	t, _ := syscall.UTF16PtrFromString(msg)
	c, _ := syscall.UTF16PtrFromString("Steam Link Sendspin")
	box.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), 0x10)
}
