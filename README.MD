# Teensy Browser #

Teensy Browser attempts to maintain a balance between functionality and resources 

1. Only tightly coupled js, layout, rendering and events are in Teensy.

2. All other processes follow unix philosophy including

     * Downloads of all resources are handled by "get"
     * Initial (non-js) parsing to DOM is handled by "parse"
     * Complex resource handling (video, animation, etc...) is handled by "splat"

## Design Decisions ##

 - quickjs was chosen as the central js component due to its balance of standards support, footprint and speed
 - clay was chosen as the layout engine for its flow-like layouts, light weight and usability with quickjs
 - lvgl was chosen as the rendering engine because its ultralight and can offload to workers on multiple backends
 - busybox was (temporarily) chosen to prototype and provide temporary commands
