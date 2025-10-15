# ScreenWeaver

Multithreaded desktop background renderer that displays videos, GIFs, and images as desktop wallpapers or compositor surfaces.

## Example on three monitors with a compositor

![example](https://github.com/user-attachments/assets/0a4fb294-068e-47a8-8beb-820d9accfb47)


## Features

- **Multiple Media Support**: Videos (MP4, WebM, MKV, AVI, MOV), animated GIFs, and static images (PNG, JPG, JPEG, BMP, TIFF, WebP, TGA)
- **Multithreaded**: Loads files with multiple threads for faster startup.
- **Multi-View Configuration**: Display multiple media files simultaneously in different screen regions
- **Playback Speed Control**: Adjustable playback speed for videos and GIFs
- **Desktop Integration**: Can create desktop windows or compositor surfaces

## Usage

### Basic Syntax

```bash
screenweaver [--compositor] PATH SPEED X Y WIDTH HEIGHT [PATH SPEED X Y WIDTH HEIGHT ...]
```

### Parameters

- `--compositor`: Optional flag to create a desktop compositor surface
- `PATH`: File or directory path containing media files
- `SPEED`: Playback speed multiplier (positive integer, default: 1)
- `X Y`: Position coordinates on screen
- `WIDTH HEIGHT`: Dimensions of the media view

### Examples

**Single fullscreen video:**

```bash
screenweaver /path/to/video.mp4 1 0 0 1920 1080
```

**Multiple media views:**

```bash
screenweaver \
    /path/to/video.mp4 2 0 0 1920 1080 \
    /path/to/animation.gif 1 1920 0 2560 1440 \
    /path/to/image.png 1 4480 0 1920 1080
```

**Desktop compositor mode:**

```bash
screenweaver --compositor /path/to/background.mp4 1 0 0 1920 1080
```
