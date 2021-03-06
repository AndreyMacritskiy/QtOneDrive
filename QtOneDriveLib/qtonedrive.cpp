#include "qtonedrive.h"
#include <QRegExp>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QDesktopServices>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QSettings>

#include "qtonedriveauthorizationdialog.h"


#define INIT_AND_CHECK_BUSY(state) Q_ASSERT_X( !isBusy(), Q_FUNC_INFO, "QtOneDrive service is busy now" );\
state_ = state;

#define INIT_AND_CHECK_BUSY_AND_AUTH(state) Q_ASSERT_X( !isBusy(), Q_FUNC_INFO, "QtOneDrive service is busy now" );\
state_ = state;\
if( !isSingIn() )  { emitError("Access Denied: User is not authorized"); return; }

#define TRY_REFRESH_TOKEN  if( isNeedRefreshToken() ) { refreshTokenRequest(); return; }

QSet<QString> QtOneDrive::exisitInstances_;

QtOneDrive::QtOneDrive( const QString& clientID, const QString& secret, const QString& userID, QObject *parent) :
    QtOneDrive(clientID,secret, QString("https://login.live.com/oauth20_desktop.srf"), userID, parent) {}

QtOneDrive::QtOneDrive(const QString& clientID, const QString& secret, const QString& redirectUri, const QString& userID, QObject *parent) :
    QObject( parent ),
    clientID_(clientID),
    secret_(secret),
    redirectURI_( redirectUri )

{
    hash_ = "QtOneDrive_" + QString( QCryptographicHash::hash( (clientID + "\n" + userID).toUtf8() ,QCryptographicHash::Md5).toHex() );
    Q_ASSERT_X( exisitInstances_.find(hash_) == exisitInstances_.end(), Q_FUNC_INFO, "You can not use ident QtOneDrive instances" );


    networkManager_ = new QNetworkAccessManager(this);
    properties_ = new QSettings(QSettings::IniFormat, QSettings::UserScope,
                                QCoreApplication::organizationName(), QCoreApplication::applicationName(), this );

    properties_->beginGroup( hash_ );
    loadProperties();
}


QtOneDrive::~QtOneDrive()
{
    if( tmp_file_ ) {
        tmp_file_->close();
        tmp_file_->deleteLater();
        tmp_file_ = nullptr;
    }
    exisitInstances_.remove( hash_ );

}

void QtOneDrive::signIn()
{
    INIT_AND_CHECK_BUSY(SingIn)

    dialog_ = new QtOneDriveAuthorizationDialog( urlSingIn(), 0 );

    connect(dialog_, &QtOneDriveAuthorizationDialog::success, [this](const QUrl &url)
    {
        qDebug() << url;
        QString code = QUrlQuery(url).queryItemValue("code");
        QString errorDesc = QUrlQuery(url).queryItemValue("error_description");

        if( !code.isEmpty() ) {
            closeAuthorizationForm();
            authorizationCode_ = code;
            getTokenRequest();
        }
        else if( !errorDesc.isEmpty() ) {
            closeAuthorizationForm();
            emitError( errorDesc );
        }
    });

    connect(dialog_, &QtOneDriveAuthorizationDialog::finished, [this](int)
    {
        if( state_ == SingIn && dialog_)
            emitError( "Authorization Cancelled" );
    });

    dialog_->show();
}

void QtOneDrive::signOut()
{
    INIT_AND_CHECK_BUSY(SingOut)

    QNetworkRequest request( urlSignOut() );
    QNetworkReply* reply  = networkManager_->get(request );

    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
        QJsonObject json = checkReplyJson(reply);
        if( !json.isEmpty() )
        {
            isSignIn_ = false;
            saveProperties();
            state_ = Empty;
            emit successSingOut();
        }
    });
}

void QtOneDrive::getUserInfo()
{
    INIT_AND_CHECK_BUSY_AND_AUTH( GetUserInfo )
    TRY_REFRESH_TOKEN

    QNetworkRequest request( urlGetUserInfo() );
    QNetworkReply* reply  = networkManager_->get(request );

    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
        QJsonObject json = checkReplyJson(reply);
        if( !json.isEmpty() )
        {
            state_ = Empty;
            emit successGetUserInfo( json );
        }
    });
}

void QtOneDrive::refreshToken()
{
    INIT_AND_CHECK_BUSY_AND_AUTH( RefreshToken )
    refreshTokenRequest();
}

void QtOneDrive::traverseFolder(const QString& folderID)
{
    INIT_AND_CHECK_BUSY_AND_AUTH( TraverseFolder );

    tmp_parentFolderId_ = folderID;

    TRY_REFRESH_TOKEN

    QNetworkRequest request( urlTraverseFolder(folderID) );
    QNetworkReply* reply  = networkManager_->get(request );

    connect(reply, &QNetworkReply::finished,  [reply, this, folderID]()
    {
        QJsonObject json = checkReplyJson(reply);
        if( !json.isEmpty() )
        {
            state_ = Empty;
            emit successTraverseFolder(json, folderID);
        }
    });
}

void QtOneDrive::getStorageInfo()
{
    INIT_AND_CHECK_BUSY_AND_AUTH( GetStorageInfo );
    TRY_REFRESH_TOKEN;

    QNetworkRequest request( urlStorageInfo() );
    QNetworkReply* reply  = networkManager_->get(request );

    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
        QJsonObject json = checkReplyJson(reply);
        if( !json.isEmpty() )
        {
            state_ = Empty;
            emit successGetStorageInfo(json);
        }
    });
}

void QtOneDrive::uploadFile(const QString& localFilePath, const QString& remoteFileName,  const QString& folderID)
{
    INIT_AND_CHECK_BUSY_AND_AUTH( UploadFile );

    tmp_parentFolderId_ = folderID;
    tmp_localFileName_ = localFilePath;
    tmp_remoteName_ = remoteFileName;

    TRY_REFRESH_TOKEN

    tmp_file_ = new QFile(localFilePath, this);
    if( !tmp_file_->open(QIODevice::ReadOnly) ) {
        emitError( QString("Unable to open file: %1").arg(localFilePath));
        return;
    }

    QNetworkRequest request( urlUploadFile(remoteFileName, folderID) );
    QNetworkReply* reply = networkManager_->put(request, tmp_file_);
    connect(reply, &QNetworkReply::finished,  [reply, this, localFilePath]()
    {
        if( tmp_file_ ) {

            tmp_file_->close();
            tmp_file_->deleteLater();
            tmp_file_ = nullptr;

            QJsonObject json = checkReplyJson(reply);

            if( !json.isEmpty() )
            {
                QString id = json.value("id").toString();
                if( !id.isEmpty() ) {
                    state_ = Empty;
                    emit successUploadFile(localFilePath, id);
                }
                else
                    emitError( "Json Error 2" );
            }
        }
    });

    connect(reply, &QNetworkReply::uploadProgress, [reply, this, localFilePath](qint64 bytesSent, qint64 bytesTotal)
    {
        qDebug() << "uploadProgress:" << bytesSent << bytesTotal;
        if( bytesTotal > 0 )
            emit progressUploadFile(localFilePath, (bytesSent*100)/bytesTotal);
    });
}

void QtOneDrive::uploadFile(QFile *file, const QString &remoteFileName, const QString &folderID)
{
    INIT_AND_CHECK_BUSY_AND_AUTH( UploadFile );

    tmp_parentFolderId_ = folderID;
    tmp_localFileName_ = file->fileName();
    tmp_remoteName_ = remoteFileName;

    TRY_REFRESH_TOKEN

    QNetworkRequest request( urlUploadFile(remoteFileName, folderID) );
    QNetworkReply* reply = networkManager_->put(request, file);
    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
            QJsonObject json = checkReplyJson(reply);

            if( !json.isEmpty() )
            {
                QString id = json.value("id").toString();
                if( !id.isEmpty() ) {
                    state_ = Empty;
                    emit successUploadFile(tmp_localFileName_, id);
                }
                else
                    emitError( "Json Error 2" );
            }
    });

    connect(reply, &QNetworkReply::uploadProgress, [reply, this](qint64 bytesSent, qint64 bytesTotal)
    {
        qDebug() << "uploadProgress:" << bytesSent << bytesTotal;
        if( bytesTotal > 0 )
            emit progressUploadFile(tmp_localFileName_, (bytesSent*100)/bytesTotal);
    });
}

void QtOneDrive::downloadFile(const QString& localFilePath, const QString& fileId)
{
    INIT_AND_CHECK_BUSY_AND_AUTH( DownloadFile );

    tmp_fileId_ = fileId;
    tmp_localFileName_ = localFilePath;

    TRY_REFRESH_TOKEN

    QNetworkRequest request( urlDownloadFile(fileId) );
    QNetworkReply* reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished,  [reply, this, fileId]()
    {
        QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if(!redirectUrl.isEmpty())
        {
            downloadFile(redirectUrl);
            return;
        }

        QJsonObject json = checkReplyJson(reply);
        if( !json.isEmpty() )
        {
            emitError("Unknown Error");
        }
    });
}

void QtOneDrive::createFolder(const QString &folderName, const QString &parentFolderId)
{
    INIT_AND_CHECK_BUSY_AND_AUTH( CreateFolder );

    tmp_parentFolderId_= parentFolderId;
    tmp_remoteName_ = folderName;

    TRY_REFRESH_TOKEN

    QNetworkRequest request( urlCreateFolder(parentFolderId) );

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject json;
    json.insert("name", folderName);

    QNetworkReply* reply = networkManager_->post(request, QJsonDocument(json).toJson() );

    connect(reply, &QNetworkReply::finished,  [reply, this, parentFolderId]()
    {
        QJsonObject json = checkReplyJson(reply);

        if( !json.isEmpty() )
        {
            QString id = json.value("id").toString();

            if( !id.isEmpty() ) {
                state_ = Empty;
                emit successCreateFolder(id);
            }
            else
                emitError( "Json Error" );
        }
    });

}

void QtOneDrive::deleteItem(const QString &id)
{
    INIT_AND_CHECK_BUSY_AND_AUTH( DeleteItem );

    tmp_fileId_ = id;

    TRY_REFRESH_TOKEN

    QNetworkRequest request( urlDeleteItem(id) );
    QNetworkReply* reply = networkManager_->sendCustomRequest(request, "DELETE");

    connect(reply, &QNetworkReply::finished,  [reply, this, id]()
    {
        if( !checkReplyJson(reply).isEmpty() ) {
            state_ = Empty;
            emit successDeleteItem(id);
        }
    });
}

void QtOneDrive::downloadFile(const QUrl &url)
{
    Q_ASSERT( state_ == DownloadFile );

    tmp_file_ = new QFile(tmp_localFileName_);
    if( !tmp_file_->open(QIODevice::ReadWrite | QIODevice::Truncate) ) {
        emitError( QString("Unable to open file: %1").arg(tmp_localFileName_));
        return;
    }

    QNetworkRequest request( url );
    QNetworkReply* reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
        if( tmp_file_ ) {
            tmp_file_->close();
            tmp_file_->deleteLater();
            tmp_file_ = nullptr;

            qDebug() << "DOWNLOAD COMPLETE:" ;

            if( !checkReplyJson(reply).isEmpty() ) {
                state_ = Empty;
                emit successDownloadFile(tmp_fileId_);
            }
        }
    });

    connect(reply, &QNetworkReply::readyRead, [reply, this]()
    {
        qDebug() << "DOWNLOAD BYTES:" << tmp_file_->size();
        tmp_file_->write( reply->readAll() );
    });

    connect(reply, &QNetworkReply::downloadProgress, [reply, this](qint64 bytesSent, qint64 bytesTotal)
    {
        qDebug() << "downloadProgress:" << bytesSent << bytesTotal;
        if( bytesTotal > 0 )
            emit progressDownloadFile(tmp_fileId_, (bytesSent*100)/bytesTotal);
    });
}

QJsonObject QtOneDrive::checkReplyJson(QNetworkReply *reply)
{
    QJsonParseError jsonError;
    QJsonObject json = QJsonDocument::fromJson(reply->readAll(), &jsonError ).object();

    qDebug() << QJsonDocument(json).toJson();

    if( !jsonError.error )
    {
        if( reply->error() == 0 ) {
            if( json.isEmpty() )
                json.insert("empty", true);
            return json;
        }

        emitError( QString("NetworkError %1:  code: %2   message: %3").arg(
                       QString::number(reply->error()),
                       json.value("error").toObject().value("code").toString("unknown"),
                       json.value("error").toObject().value("message").toString("unknown") ) );
    }
    else
    {
        if( reply->error() == 0 ) {
            if( json.isEmpty() ) json.insert("empty", true);
            return json;
        }

        emitError( QString("NetworkError %1: description: %2").arg(
                       QString::number(reply->error()), reply->errorString() ) );
    }


    return QJsonObject();

}

void QtOneDrive::onRefreshTokenFinished()
{
    switch( state_ )
    {
    case GetUserInfo:
        state_ = Empty;
        getUserInfo();
        break;

    case RefreshToken:
        state_ = Empty;
        emit successRefreshToken();
        break;

    case GetStorageInfo:
        state_ = Empty;
        emit getStorageInfo();
        break;

    case TraverseFolder:
        state_ = Empty;
        traverseFolder(tmp_parentFolderId_);
        break;

    case UploadFile:
        state_ = Empty;
        uploadFile(tmp_localFileName_, tmp_remoteName_, tmp_parentFolderId_);
        break;

    case DownloadFile:
        state_ = Empty;
        downloadFile(tmp_localFileName_, tmp_fileId_);
        break;

    case DeleteItem:
        state_ = Empty;
        deleteItem( tmp_fileId_);
        break;

    case CreateFolder:
        state_ = Empty;
        createFolder(tmp_remoteName_, tmp_parentFolderId_);
        break;

    default:
        break;
    }
}

void QtOneDrive::refreshTokenRequest()
{
    tokenTime_ = QDateTime::currentDateTime();

    QNetworkRequest request( urlGetToken() );
    request.setHeader( QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded" );
    QNetworkReply* reply  = networkManager_->post(request, postRefreshToken().query(QUrl::FullyEncoded).toUtf8() );

    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
        QJsonObject json = checkReplyJson(reply);

        if( !json.isEmpty() )
        {
            accessToken_ = json["access_token"].toString();
            refreshToken_ = json["refresh_token"].toString();
            authenticationToken_ = json["authentication_token"].toString();
            expiredTokenTime_ = tokenTime_.addSecs( json["expires_in"].toInt() - 60 );
            saveProperties();
            onRefreshTokenFinished();
        }
    });

}

void QtOneDrive::getTokenRequest()
{
    tokenTime_ = QDateTime::currentDateTime();

    QNetworkRequest request( urlGetToken() );
    request.setHeader( QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded" );

    QNetworkReply* reply = networkManager_->post(request, postGetToken().query(QUrl::FullyEncoded).toUtf8() );

    connect(reply, &QNetworkReply::finished,  [reply, this]()
    {
        QJsonObject json = checkReplyJson(reply);

        if( !json.isEmpty() )
        {
            accessToken_ = json["access_token"].toString();
            refreshToken_ = json["refresh_token"].toString();
            authenticationToken_ = json["authentication_token"].toString();
            expiredTokenTime_ = tokenTime_.addSecs( json["expires_in"].toInt() - 60 );
            isSignIn_ = true;

            saveProperties();
             state_ = Empty;
            emit successSignIn();

        }
    });
}

void QtOneDrive::closeAuthorizationForm()
{
    if( QtOneDriveAuthorizationDialog::isExists() && dialog_ ) {
        dialog_->setNeedToClose(true);
        dialog_ = nullptr;
    }
}

QString QtOneDrive::debugInfo() const
{
    QString info;
    QTextStream stream(&info);

    stream << "State: " << state_ << "\n";
    stream << "IsSingIn: " << isSingIn() << "\n";
    stream << "clientID: " << clientID_ << "\n";
    stream << "redirectURI: " << redirectURI_ << "\n";
    stream << "secret: " << secret_ << "\n";
    stream << "expiredTokenTime: " << expiredTokenTime_.toString() << "\n";
    stream << "Current Time: " << QDateTime::currentDateTime().toString() << "\n";
    stream << "authorizationCode: " << authorizationCode_ << "\n";
    stream << "accessToken: " << accessToken_ << "\n";
    stream << "refreshToken: " << refreshToken_ << "\n";
    stream << "authenticationToken: " << authenticationToken_ << "\n";
    return info;
}

QUrl QtOneDrive::urlSingIn() const
{
    QUrl url;
    url.setUrl("https://login.live.com/oauth20_authorize.srf");
    QUrlQuery query;
    query.addQueryItem("client_id", clientID_);
    query.addQueryItem("scope", "wl.signin wl.basic wl.offline_access wl.skydrive_update wl.skydrive");
    query.addQueryItem("response_type", "code");
    query.addQueryItem("redirect_uri", redirectURI_ );
    url.setQuery( query );
    return url;
}

QUrl QtOneDrive::urlSignOut() const
{
    QUrl url( "https://login.live.com/oauth20_logout.srf" );
    QUrlQuery query;
    query.addQueryItem("client_id", clientID_);
    query.addQueryItem("redirect_uri", redirectURI_ );
    url.setQuery( query );
    return url;
}

QUrl QtOneDrive::urlStorageInfo() const
{
    QUrl url("https://apis.live.net/v5.0/me/skydrive/quota");
    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    url.setQuery( query );
    return url;
}

QUrl QtOneDrive::urlGetToken() const
{
    return QUrl("https://login.live.com/oauth20_token.srf");
}

QUrl QtOneDrive::urlGetUserInfo() const
{
    QUrl url("https://apis.live.net/v5.0/me");
    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    url.setQuery( query );
    return url;
}

QUrl QtOneDrive::urlDeleteItem(const QString &id) const
{
    QUrl url( QString("https://apis.live.net/v5.0/%1").arg(id) );
    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    url.setQuery( query );
    return url;
}

QUrl QtOneDrive::urlTraverseFolder(const QString& parentFolderId) const
{
    QUrl url("https://apis.live.net/v5.0/me/skydrive");

    if( parentFolderId != "" )
        url = QUrl( QString("https://apis.live.net/v5.0/%1/files").arg(parentFolderId) );

    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    url.setQuery( query );
    return url;
}

QUrl QtOneDrive::urlUploadFile(const QString& remoteFileName, const QString& folderId) const
{
    QUrl url("https://apis.live.net/v5.0/me/skydrive/files/" + remoteFileName );

    if( folderId != "" )
        url = QUrl( QString("https://apis.live.net/v5.0/%1/files/%2").arg(folderId, remoteFileName) );

    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    url.setQuery( query );
    qDebug() << url.toString();
    return url;
}

QUrl QtOneDrive::urlDownloadFile(const QString& fileId) const
{
    QUrl url( QString("https://apis.live.net/v5.0/%1/content").arg(fileId) );

    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    query.addQueryItem("download", "true");
    url.setQuery( query );
    qDebug() << url.toString();
    return url;
}

QUrl QtOneDrive::urlCreateFolder(const QString &id) const
{
    QUrl url("https://apis.live.net/v5.0/me/skydrive");

    if( id != "" )
        url = QUrl( QString("https://apis.live.net/v5.0/%1/files").arg(id) );

    QUrlQuery query;
    query.addQueryItem("access_token", accessToken_);
    url.setQuery( query );
    qDebug() << url.toString();
    return url;
}

QUrlQuery QtOneDrive::postGetToken() const
{
    QUrlQuery query;
    query.addQueryItem("client_id", clientID_);
    query.addQueryItem("redirect_uri", redirectURI_ );
    query.addQueryItem("client_secret", secret_);
    query.addQueryItem("code", authorizationCode_);
    query.addQueryItem("grant_type", "authorization_code" );
    qDebug() << query.toString();
    return query;
}

QUrlQuery QtOneDrive::postRefreshToken() const
{
    QUrlQuery query;
    query.addQueryItem("client_id", clientID_);
    query.addQueryItem("redirect_uri", redirectURI_ );
    query.addQueryItem("client_secret", secret_);
    query.addQueryItem("refresh_token", refreshToken_);
    query.addQueryItem("grant_type", "refresh_token" );
    return query;
}

void QtOneDrive::saveProperties()
{
    qDebug() << Q_FUNC_INFO;
    properties_->setValue("authorization_code", authorizationCode_);
    properties_->setValue("access_token", accessToken_);
    properties_->setValue("refresh_token", refreshToken_);
    properties_->setValue("authentication_token", authenticationToken_);
    properties_->setValue("expired_token_time", expiredTokenTime_);
    properties_->setValue("is_signin", isSignIn_);
}

void QtOneDrive::loadProperties()
{
    authorizationCode_ = properties_->value("authorization_code", "").toString();
    accessToken_ = properties_->value("access_token", "").toString();
    refreshToken_ = properties_->value("refresh_token", "").toString();
    authenticationToken_ = properties_->value("authentication_token", "").toString();
    expiredTokenTime_ = properties_->value("expired_token_time", QDateTime() ).toDateTime();
    isSignIn_ = properties_->value("is_signin", false).toBool();
}

void QtOneDrive::emitError(const QString& errorDesc)
{
    OneDriveState state = this->state_;
    state_ = Empty;
    qDebug() << "\nERROR: " << state << "\n" <<  errorDesc;

    if( state == SingIn ) emit errorSignIn(errorDesc);
    if( state == SingOut ) emit errorSignOut(errorDesc);
    if( state == GetUserInfo ) emit errorGetUserInfo(errorDesc);
    if( state == RefreshToken ) emit errorRefreshToken(errorDesc);
    if( state == UploadFile ) emit errorUploadFile(errorDesc);
    if( state == DownloadFile ) emit errorDownloadFile(errorDesc);
    if( state == DeleteItem ) emit errorDeleteItem(errorDesc);
    if( state == CreateFolder ) emit errorCreateFolder(errorDesc);

    emit error(errorDesc);
}

bool QtOneDrive::isNeedRefreshToken() const
{
    return QDateTime::currentDateTime() > expiredTokenTime_;
}
