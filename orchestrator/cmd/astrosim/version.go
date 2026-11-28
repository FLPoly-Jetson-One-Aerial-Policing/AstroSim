package main

// version is the orchestrator version. The release build sets it with
//
//	-ldflags "-X main.version=<git describe output>"
var version = "dev"

// imageRepository and imageTag select the component images of a release
// build. CI sets both:
//
//	-ldflags "-X main.imageRepository=ghcr.io/flpoly-jetson-one-aerial-policing -X main.imageTag=0.1.0"
//
// Left empty (a development build) the images are the locally built
// astrosim-engine and astrosim-fc, which are never downloaded.
var (
	imageRepository = ""
	imageTag        = ""
)
