Name:           paint
Version:        0.3.0
Release:        1%{?dist}
Summary:        Clean GNOME Paint application built with GTK 4 and Libadwaita

License:        GPL-3.0-or-later
URL:            https://github.com/Spondle/Draw
Source0:        %{url}/archive/v%{version}/Draw-%{version}.tar.gz

BuildRequires:  appstream
BuildRequires:  desktop-file-utils
BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(gtk4) >= 4.12
BuildRequires:  pkgconfig(libadwaita-1) >= 1.6
BuildRequires:  pkgconfig(pangocairo)

Requires:       hicolor-icon-theme

%description
Paint is a fast, distraction-free raster drawing and painting application
built with GTK 4, Libadwaita, and Cairo in the style of GNOME Text Editor
and Ptyxis.

%prep
%autosetup -n Draw-%{version}

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.spondle.Paint.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/io.github.spondle.Paint.metainfo.xml

%files
%license README.md
%doc CHANGELOG.md
%{_bindir}/paint
%{_datadir}/applications/io.github.spondle.Paint.desktop
%{_datadir}/metainfo/io.github.spondle.Paint.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.spondle.Paint.svg
%{_datadir}/icons/hicolor/symbolic/apps/io.github.spondle.Paint-symbolic.svg

%changelog
* Sat Nov 21 2026 spondle <noreply@example.invalid> - 0.3.0-1
- Add multi-layer editing, blend modes, and collapsible Layers sidebar
- Add Selection, Text, Eyedropper, Resize/Scale, Rotate/Flip, Fit to Window, and Pixel Grid tools
- Add brush styles, Recent Colors, Block Arrow shape, and Spacebar shape repositioning
- Add customizable keyboard shortcuts and startup window/canvas sizing preferences

* Wed Nov 18 2026 spondle <noreply@example.invalid> - 0.2.0-1
- Add full transparency support for PNG and WebP images with a checkerboard canvas backdrop
- Add Transparency tool (Brush, Fill, and Clear to Transparent)
- Add Line tool and Shapes tool (8 shapes with outline/filled modes and Shift snapping)
- Add Clear Canvas button to the floating toolbar

* Wed Nov 18 2026 spondle <noreply@example.invalid> - 0.1.1-1
- Improve large-canvas drawing and zoom performance without tile seams
- Add touchscreen pinch-to-zoom and focal-point zoom anchoring

* Tue Nov 17 2026 spondle <noreply@example.invalid> - 0.1.0-1
- Initial release
