# Nautilus Image Converter (Enhanced)

The **Nautilus-Image-Converter** extension allows you to resize, rotate, and convert images between formats directly from the right-click context menu in the Nautilus (GNOME Files) file manager.

This repository is an enhanced fork of the original extension, featuring new improvements to make image manipulation even easier for modern workflows.

## ✨ Features

Right-click one or more images and choose **Resize/convert image…**. A single window lets you:

* **🗂️ Use profiles:** pick a ready-made profile — *Web* (WebP, fit in 1280×960), *Email*, *Social media*, *Thumbnail*, *Under 1 MB*, *Lossless PNG* — or tweak the settings and save them back to the profile, or as a new one. Profiles are stored in `~/.config/nautilus-image-converter/profiles.ini`.
* **📏 Resize:** keep the original size, scale by a percentage, fit within a box (optionally never enlarging), or force an exact size.
* **🔄 Convert & compress:** keep the format or convert to **JPEG, PNG, WebP, AVIF, GIF or PDF**, set the quality, strip metadata, and optionally cap the file size (JPEG and WebP).
* **✂️ Crop & rotate visually:** drag a crop rectangle (free or locked to a ratio), rotate by 90°, flip, and straighten with a slider. Straightening automatically crops away the empty corners.
* **👁️ Live preview:** see the actual result — dimensions, file size and the change compared with the original — before converting anything. Switch to *Actual size* to inspect compression artefacts.
* **📄 Combine to PDF:** select several images and combine them into a single PDF (`image1_to_image3.pdf`).
* **💾 Output:** write a new file with a suffix (e.g. `photo-web.webp`), or replace the original. When the format changes, the original is moved to the trash.

With several images selected, you can browse through them in the preview; the crop and rotation apply to all of them (the crop is relative to each image's size).

## 📥 Installation

To build and install the extension from source, run the following commands:

```bash
# 1. Install dependencies (Ubuntu/Debian example)
sudo apt install libgtk-4-dev libnautilus-extension4 libnautilus-extension-dev gettext meson ninja-build imagemagick

# 2. Clone this repository
git clone https://github.com/Ameen-Sha-Cheerangan/nautilus-image-converter-gnome43plus.git
cd nautilus-image-converter-gnome43plus

# 3. Build the project
meson build
ninja -C build

# 4. Install
sudo ninja install -C build
```

**Important:** Once installed, you must completely restart Nautilus to see the extension in your context menu:
```bash
nautilus -q
```

## 🗑️ Uninstallation

Navigate to your project directory (`nautilus-image-converter`) where you built the project:

```bash
cd nautilus-image-converter
sudo ninja uninstall -C build
```

## 🕰️ Older GNOME Versions (GNOME < 43)

Are you using an older version of GNOME (e.g., Ubuntu 22.04 or earlier)? This repository requires GNOME 43+ and GTK4.

For older systems, please use the **[Legacy Version](https://github.com/Ameen-Sha-Cheerangan/nautilus-image-converter-legacy)** which includes the Target File Size feature, but some features might be missing (note: Format Conversion is only available in this GTK4 version).

## 🤝 Contributing

Patches, bug reports, and feature requests are always welcome! Feel free to open an Issue or a Pull Request on this repository.

## ⭐ Show Your Support

If you found this extension helpful, please consider giving this repository a star on GitHub! It helps others find the project.



## ☕ Support the developer

If this tool helped you, consider supporting its development!

### 🌐 International
You can support me instantly via **Ko-fi**.

<a href="https://ko-fi.com/ameen_sha" target="_blank">
  <img src="https://storage.ko-fi.com/cdn/brandasset/kofi_button_blue.png" alt="Buy Me a Coffee at ko-fi.com" height="36" />
</a>

### 🇮🇳 India (UPI)
You can support directly via any UPI app (GPay, PhonePe, Paytm) using the QR code or ID below:
<details>
  <summary>View UPI payment options</summary>
<div align="center">
  <img src="https://raw.githubusercontent.com/Ameen-Sha-Cheerangan/.github/main/assets/upi-qr.jpg" alt="UPI QR Code" width="220"/>
  <br/>
  <b>UPI ID:</b> <code>ameenshahcheerangan-1@okicici</code>
</div>
</details>

<br/>

> **📱 Viewing on mobile?** 
> * **Direct Link:** [Click here to open your UPI app](https://upi.pe/ameenshahcheerangan-1@okicici?pn=Ameen+Sha+C)
> * **Manual Scan:** Take a screenshot of the QR code above and upload it directly inside your UPI app (GPay, PhonePe, Paytm, etc.).

## 📜 Credits & Acknowledgments

This project is a fork of [Corey Berla's repository](https://gitlab.gnome.org/coreyberla/nautilus-image-converter), which ported the original extension created by **Jürg Billeter** ([GNOME Archive](https://gitlab.gnome.org/Archive/nautilus-image-converter)) to GTK4 and Meson.
