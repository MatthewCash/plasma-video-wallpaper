import QtQuick
import org.kde.plasma.plasmoid
import org.kde.videowallpaper

WallpaperItem {
    id: root
    property string source: root.configuration.source || ""

    MpvItem {
        id: player
        anchors.fill: parent
        source: root.source
    }
}
