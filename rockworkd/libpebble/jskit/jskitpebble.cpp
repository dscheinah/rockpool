#include <QUrl>
#include <QCryptographicHash>
#include <QSettings>
#include <QJsonObject>

#include "jskitpebble.h"
#include "jskitxmlhttprequest.h"
#include "jskitwebsocket.h"
#include "../timelinemanager.h"
#include "../timelinesync.h"
#include "../appglances.h"
static const char *token_salt = "0feeb7416d3c4546a19b04bccd8419b1";

JSKitPebble::JSKitPebble(const AppInfo &info, JSKitManager *mgr, QObject *parent) :
    QObject(parent),
    l(metaObject()->className()),
    m_appInfo(info),
    m_mgr(mgr)
{
}

void JSKitPebble::addEventListener(const QString &type, QJSValue function)
{
    m_listeners[type].append(function);
}

void JSKitPebble::removeEventListener(const QString &type, QJSValue function)
{
    if (!m_listeners.contains(type)) return;

    QList<QJSValue> &callbacks = m_listeners[type];
    for (QList<QJSValue>::iterator it = callbacks.begin(); it != callbacks.end(); ) {
        if (it->strictlyEquals(function)) {
            it = callbacks.erase(it);
        } else {
            ++it;
        }
    }

    if (callbacks.empty()) {
        m_listeners.remove(type);
    }
}

void JSKitPebble::showSimpleNotificationOnPebble(const QString &title, const QString &body)
{
    qCDebug(l) << "showSimpleNotificationOnPebble" << title << body;
    QJsonObject pin,layout;
    pin.insert("id", QString("%1:%2").arg(m_appInfo.shortName(), QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()));
    pin.insert("dataSource", QString("%1:%2").arg(m_appInfo.uuid().toString().mid(1,36), m_appInfo.uuid().toString().mid(1,36)));
    pin.insert("type", QString("notification"));
    pin.insert("source", QString(m_appInfo.shortName()));
    layout.insert("title", title);
    layout.insert("body", body);
    layout.insert("type", QString("genericNotification"));
    pin.insert("layout", layout);
    emit m_mgr->appNotification(pin);
}

uint JSKitPebble::sendAppMessage(QJSValue message, QJSValue callbackForAck, QJSValue callbackForNack)
{
    QVariantMap data = message.toVariant().toMap();
    QPointer<JSKitPebble> pebbObj = this;
    uint transactionId = m_mgr->m_appmsg->nextTransactionId();

    qCDebug(l) << "sendAppMessage" << data;

    m_mgr->m_appmsg->send(
        m_appInfo.uuid(),
        data,
        [this, pebbObj, transactionId, callbackForAck]() mutable {
            if (pebbObj.isNull()) return;

            if (callbackForAck.isCallable()) {
                QJSValue event = pebbObj->buildAckEventObject(transactionId);
                QJSValue result = callbackForAck.call(QJSValueList({event}));

                if (result.isError()) {
                    qCWarning(l) << "error while invoking ACK callback"
                        << callbackForAck.toString() << ":"
                        << JSKitManager::describeError(result);
                }
            }
        },
        [this, pebbObj, transactionId, callbackForNack]() mutable {
            if (pebbObj.isNull()) return;

            if (callbackForNack.isCallable()) {
                QJSValue event = pebbObj->buildAckEventObject(transactionId, "NACK from watch");
                QJSValue result = callbackForNack.call(QJSValueList({event}));

                if (result.isError()) {
                    qCWarning(l) << "error while invoking NACK callback"
                        << callbackForNack.toString() << ":"
                        << JSKitManager::describeError(result);
                }
            }
        }
    );

    return transactionId;
}

void JSKitPebble::appGlanceReload(QJSValue slices, QJSValue callbackForAck, QJSValue callbackForNack)
{
    QVariantList vs = slices.toVariant().toList();
    QList<AppGlances::Slice> sls;
    foreach(const QVariant sv, vs) {
        if(sv.canConvert(QMetaType::QJsonObject)) {
            QJsonObject so=sv.toJsonObject();
            if(so.contains("layout")) {
                QList<TimelineAttribute> tas;
                tas.append(m_mgr->pebble()->timeline()->parseAttribute("timestamp",so.value("expirationTime")));
                QJsonObject layout = so.value("layout").toObject();
                for(QJsonObject::const_iterator it=layout.begin(); it != layout.end(); it++) {
                    tas.append(m_mgr->pebble()->timeline()->parseAttribute(it.key(),it.value()));
                }
                sls.append(AppGlances::Slice(AppGlances::TypeIconSubtitle,tas));
            }
        }

    }
    m_mgr->pebble()->appGlances()->reloadAppGlances(m_mgr->currentApp().uuid(),sls,[this,slices,callbackForAck,callbackForNack](int cmd, int ack)mutable{
        if(cmd==BlobDB::OperationInsert) {
            QJSValue o = m_mgr->engine()->newObject();
            if(ack==BlobDB::StatusSuccess) {
                o.setProperty("success",true);
                if(callbackForAck.isCallable())
                    callbackForAck.call(QJSValueList() << slices << o);
            } else {
                o.setProperty("success",false);
                if(callbackForNack.isCallable())
                    callbackForNack.call(QJSValueList() << slices << o);
            }
        }
    });
}

void JSKitPebble::getTimelineToken(QJSValue successCallback, QJSValue failureCallback)
{
    getTokenInternal([this,successCallback]()mutable{
        if(successCallback.isCallable()) {
            successCallback.call(QJSValueList({m_timelineToken}));
        }
    },failureCallback);
}

void JSKitPebble::timelineSubscribe(const QString &topic, QJSValue successCallback, QJSValue failureCallback)
{
    getTokenInternal([this,topic,&successCallback,&failureCallback](){
        m_mgr->pebble()->tlSync()->topicSubscribe(m_timelineToken,topic,[this,successCallback,topic](const QString &ok)mutable{
            qCDebug(l) << "Successfully subscribed to" << topic << ok;
            if(successCallback.isCallable()) {
                successCallback.call(QJSValueList({ok}));
            }
        },[this,topic,failureCallback](const QString &err)mutable{
            qCDebug(l) << "Cannot subscribe to" << topic << err;
            if (failureCallback.isCallable()) {
                failureCallback.call(QJSValueList({err}));
            }
        });
    },failureCallback);
}

void JSKitPebble::timelineUnsubscribe(const QString &topic, QJSValue successCallback, QJSValue failureCallback)
{
    getTokenInternal([this,topic,&successCallback,failureCallback]()mutable{
        m_mgr->pebble()->tlSync()->topicUnsubscribe(m_timelineToken,topic,[this,successCallback,topic](const QString &ok)mutable{
            qCDebug(l) << "Successfully unsubscribed from" << topic << ok;
            if(successCallback.isCallable()) {
                successCallback.call(QJSValueList({ok}));
            }
        },[this,topic,failureCallback](const QString &err)mutable{
            qCDebug(l) << "Cannot unsubscribe from" << topic << err;
            if (failureCallback.isCallable()) {
                failureCallback.call(QJSValueList({err}));
            }
        });
    },failureCallback);
}

void JSKitPebble::timelineSubscriptions(QJSValue successCallback, QJSValue failureCallback)
{
    getTokenInternal([this,&successCallback,&failureCallback](){
        m_mgr->pebble()->tlSync()->getSubscriptions(m_timelineToken,[this,successCallback](const QStringList &topics)mutable{
            qCDebug(l) << "Successfully fetched subscriptions:" << topics.join(", ");
            if(successCallback.isCallable()) {
                QJSValue argArray = m_mgr->engine()->newArray(topics.size());
                for (int i = 0; i < topics.size(); i++) {
                    argArray.setProperty(i, topics.at(i));
                }
                successCallback.call(QJSValueList({argArray}));
            }
        },[failureCallback](const QString &err)mutable{
            if (failureCallback.isCallable()) {
                failureCallback.call(QJSValueList({err}));
            }
        });
    },failureCallback);
}

template<typename Func>
void JSKitPebble::getTokenInternal(Func ack, QJSValue &failureCallback)
{
    if(!m_timelineToken.isEmpty()) {
        ack();
        return;
    }
    m_mgr->pebble()->tlSync()->getTimelineToken(m_appInfo.uuid(),
       [this,failureCallback,ack](const QString &ret)mutable{
        m_timelineToken = ret;
        if(m_timelineToken.isEmpty()) {
            if (failureCallback.isCallable()) {
                failureCallback.call(QJSValueList({"Unknown Error: token is empty"}));
            }
        } else {
            ack();
        }
    }, [failureCallback](const QString &err)mutable{
        if (failureCallback.isCallable()) {
            failureCallback.call(QJSValueList({err}));
        }
    });
}


QString JSKitPebble::getAccountToken() const
{
    // We do not have any account system, so we just fake something up.
    QCryptographicHash hasher(QCryptographicHash::Md5);

    hasher.addData(token_salt, strlen(token_salt));
    hasher.addData(m_appInfo.uuid().toByteArray());

    QSettings settings;
    QString token = settings.value("accountToken").toString();

    if (token.isEmpty()) {
        token = QUuid::createUuid().toString();
        qCDebug(l) << "created new account token" << token;
        settings.setValue("accountToken", token);
    }

    hasher.addData(token.toLatin1());

    QString hash = hasher.result().toHex();
    qCDebug(l) << "returning account token" << hash;

    return hash;
}

QString JSKitPebble::getWatchToken() const
{
    QCryptographicHash hasher(QCryptographicHash::Md5);

    hasher.addData(token_salt, strlen(token_salt));
    hasher.addData(m_appInfo.uuid().toByteArray());
    hasher.addData(m_mgr->m_pebble->serialNumber().toLatin1());

    QString hash = hasher.result().toHex();
    qCDebug(l) << "returning watch token" << hash;

    return hash;
}

QJSValue JSKitPebble::getActiveWatchInfo() const
{
    QJSValue watchInfo = m_mgr->m_engine->newObject();

    watchInfo.setProperty("platform", m_mgr->m_pebble->platformName());

    switch (m_mgr->m_pebble->model()) {
    case ModelTintinWhite:
        watchInfo.setProperty("model", "pebble_white");
        break;

    case ModelTintinRed:
        watchInfo.setProperty("model", "pebble_red");
        break;

    case ModelTintinOrange:
        watchInfo.setProperty("model", "pebble_orange");
        break;

    case ModelTintinGrey:
        watchInfo.setProperty("model", "pebble_grey");
        break;

    case ModelBiancaSilver:
        watchInfo.setProperty("model", "pebble_steel_silver");
        break;

    case ModelBiancaBlack:
        watchInfo.setProperty("model", "pebble_steel_black");
        break;

    case ModelTintinBlue:
        watchInfo.setProperty("model", "pebble_blue");
        break;

    case ModelTintinGreen:
        watchInfo.setProperty("model", "pebble_green");
        break;

    case ModelTintinPink:
        watchInfo.setProperty("model", "pebble_pink");
        break;

    case ModelSnowyWhite:
        watchInfo.setProperty("model", "pebble_time_white");
        break;

    case ModelSnowyBlack:
        watchInfo.setProperty("model", "pebble_time_black");
        break;

    case ModelSnowyRed:
        watchInfo.setProperty("model", "pebble_time_read");
        break;

    case ModelBobbySilver:
        watchInfo.setProperty("model", "pebble_time_steel_silver");
        break;

    case ModelBobbyBlack:
        watchInfo.setProperty("model", "pebble_time_steel_black");
        break;

    case ModelBobbyGold:
        watchInfo.setProperty("model", "pebble_time_steel_gold");
        break;

    case ModelSpalding14Silver:
        watchInfo.setProperty("model", "pebble_time_round_silver_14mm");
        break;

    case ModelSpalding14Black:
        watchInfo.setProperty("model", "pebble_time_round_black_14mm");
        break;

    case ModelSpalding20Silver:
        watchInfo.setProperty("model", "pebble_time_round_silver_20mm");
        break;

    case ModelSpalding20Black:
        watchInfo.setProperty("model", "pebble_time_round_black_20mm");
        break;

    case ModelSpalding14RoseGold:
        watchInfo.setProperty("model", "pebble_time_round_rose_gold_14mm");
        break;

    case ModelSilkHrAqua:
        watchInfo.setProperty("model", "pebble_2_hr_aqua");
        break;

    case ModelSilkHrFlame:
        watchInfo.setProperty("model", "pebble_2_hr_flame");
        break;

    case ModelSilkHrLime:
        watchInfo.setProperty("model", "pebble_2_hr_lime");
        break;

    case ModelSilkHrWhite:
        watchInfo.setProperty("model", "pebble_2_hr_white");
        break;

    case ModelSilkSeBlack:
        watchInfo.setProperty("model", "pebble_2_se_black");
        break;

    case ModelSilkSeWhite:
        watchInfo.setProperty("model", "pebble_2_se_white");
        break;

    case ModelRobertBlack:
        watchInfo.setProperty("model", "pebble_time_2_black");
        break;

    case ModelRobertGold:
        watchInfo.setProperty("model", "pebble_time_2_gold");
        break;

    case ModelRobertSilver:
        watchInfo.setProperty("model", "pebble_time_2_silver");
        break;

    default:
        watchInfo.setProperty("model", "pebble_black");
        break;
    }

    watchInfo.setProperty("language", m_mgr->m_pebble->language());

    QJSValue firmware = m_mgr->m_engine->newObject();
    QString version = m_mgr->m_pebble->softwareVersion().remove("v");
    QStringList versionParts = version.split(".");

    if (versionParts.count() >= 1) {
        firmware.setProperty("major", versionParts[0].toInt());
    }

    if (versionParts.count() >= 2) {
        firmware.setProperty("minor", versionParts[1].toInt());
    }

    if (versionParts.count() >= 3) {
        if (versionParts[2].contains("-")) {
            QStringList patchParts = version.split("-");
            firmware.setProperty("patch", patchParts[0].toInt());
            firmware.setProperty("suffix", patchParts[1]);
        } else {
            firmware.setProperty("patch", versionParts[2].toInt());
            firmware.setProperty("suffix", "");
        }
    }

    watchInfo.setProperty("firmware", firmware);
    return watchInfo;
}

void JSKitPebble::openURL(const QUrl &url)
{
    emit m_mgr->openURL(m_appInfo.uuid().toString(), url.toString());
}

QJSValue JSKitPebble::createXMLHttpRequest()
{
    JSKitXMLHttpRequest *xhr = new JSKitXMLHttpRequest(m_mgr->engine());
    return m_mgr->engine()->newQObject(xhr);
}

QJSValue JSKitPebble::createWebSocket(const QString &url, const QJSValue &protocols)
{
    JSKitWebSocket *ws = new JSKitWebSocket(m_mgr->engine(), url, protocols);
    return m_mgr->engine()->newQObject(ws);
}


QJSValue JSKitPebble::buildAckEventObject(uint transaction, const QString &message) const
{
    QJSEngine *engine = m_mgr->engine();
    QJSValue eventObj = engine->newObject();
    QJSValue dataObj = engine->newObject();

    dataObj.setProperty("transactionId", engine->toScriptValue(transaction));
    eventObj.setProperty("data", dataObj);

    if (!message.isEmpty()) {
        QJSValue errorObj = engine->newObject();

        errorObj.setProperty("message", engine->toScriptValue(message));
        eventObj.setProperty("error", errorObj);
    }

    return eventObj;
}

void JSKitPebble::invokeCallbacks(const QString &type, const QJSValueList &args)
{
    if (!m_listeners.contains(type)) return;
    QList<QJSValue> &callbacks = m_listeners[type];

    for (QList<QJSValue>::iterator it = callbacks.begin(); it != callbacks.end(); ++it) {
        qCDebug(l) << "invoking callback" << type << it->toString();
        QJSValue result = it->call(args);
        if (result.isError()) {
            qCWarning(l) << "error while invoking callback"
                << type << it->toString() << ":"
                << JSKitManager::describeError(result);
        }
    }
}
