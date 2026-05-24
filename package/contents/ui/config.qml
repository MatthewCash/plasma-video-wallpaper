import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    property alias cfg_source: sourceField.text

    Controls.TextField {
        id: sourceField
        Kirigami.FormData.label: i18n("Video source:")
        placeholderText: i18n("file:///path/to/video.mp4")
    }
}
