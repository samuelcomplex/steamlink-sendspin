//go:build !linux && !windows

package main

func arpTable() map[string]string { return map[string]string{} }
