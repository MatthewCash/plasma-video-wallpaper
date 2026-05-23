#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include "mpvitem.h"

class Plugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)
public:
    void registerTypes(const char *) override
    {
        qmlRegisterType<MpvItem>("org.kde.videowallpaper", 1, 0, "MpvItem");
    }
};

#include "plugin.moc"
