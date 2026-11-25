# Design notes

## Why the image is flipped, and not the world

A mirror has determinant -1, while engine transforms are rotation plus translation. Mirroring the actual world would need mirrored meshes, collision, navigation and AI data for every location, and Glacier normalizes or clamps negative transforms set at runtime.

Negating the camera projection is not an option either. It reverses the winding order, so the cull mode would have to change, and in D3D12 that is baked into the pipeline state objects. Shadow passes render with light cameras and need the original cull mode, so a global swap does not work. The camera data handed to DLSS would have to stay consistent as well.

Flipping the finished image is a pure screen space operation. Lighting, shadows, reflections, TAA and DLSS upscaling are not affected.

The price is Frame Generation, whose motion vectors belong to the unmirrored image.

## Where the flip happens

The flip has to happen after the world and before the UI. The game draws its UI in `DrawScaleform`, into a command list that is still being recorded. A command list of the mod's own would execute before the commands the engine has not submitted yet, so the flip is recorded into the engine's list.

`CommandListHooks` finds that list: the first draw call on a thread inside an armed `DrawScaleform` arrives with it as `this`. D3D12 has no getters for the pipeline state and root signature, so the hooks remember them per list, and the flip restores them afterwards.

The pass itself copies the texture to a raw buffer, swaps the pixels of each row in a compute shader, and copies the result back. Working on a buffer makes it independent of the texture format and needs no descriptors, so only the pipeline state and the compute root signature have to be restored.

A command list that fails to close makes the engine abort. So before the engine's list is touched for a new target, the same commands are rehearsed on a private list that is closed but never executed. If that fails, the mod falls back to flipping whole frames at present, which mirrors the UI but cannot break the engine.

## Why 47 is mirrored too

Keeping 47 unmirrored is possible in principle, but the simulation stays unmirrored and his animations hit contact points there. Displayed, a hand would land off by twice its sideways distance, which shows at doors, ledges, takedowns and pickups.

## The HUD

The UI is not mirrored, so everything in it that refers to the world has to be moved.

- Most world-anchored elements get their screen position from one engine function, `0xA15970`. Mirroring its result moves them and leaves their text alone.
- Other elements are placed around 47 or the screen center. They go through the generic setters of UI controls, which are told apart by their return address.
- The two maps are Scaleform containers. A negative scale mirrors a whole map with its markers, and the markers are then turned back one by one to stay readable. Markers that turn with their entity stay mirrored, because that is what reverses their direction.
- The picture-in-picture is part of the world image, so it is drawn in the opposite corner and the flip brings it back. Its rectangle is only sent while a level loads, so toggling the mod has to send it again.

## The toggle key

The key is read with `GetAsyncKeyState` rather than the game's input system. Bindings there cannot be changed once registered and only know the engine's own key names, which would make a freely chosen key hard to validate.

## Threads

The SDK calls the window on the render thread, and that is also where the toggle key is polled, so that everything installing hooks after startup runs on one thread. Only `HudMirror::RefreshPip` has to run on the game thread, because it writes to the engine's render queue. Settings are atomic since hooks read them from the game, render and audio threads.

Under the loader lock, in `Init`, hooks are only created. Their trampolines have to be close to the game's code, and that address space fills up as the game runs, so creating them early matters. Enabling them suspends other threads and happens later.
