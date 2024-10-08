# Свойства JDBC-драйвера

JDBC-драйвер для {{ ydb-short-name }} поддерживает следующие конфигурационные свойства, которые можно указать в [JDBC URL](#jdbc-url) или передать через дополнительные свойства:

* `saFile` — ключ сервисной учётной записи (Service Account Key). Значением свойства может быть содержимое ключа или путь к файлу с ключом. {#saFile}

* `iamEndpoint` — IAM-эндпойнт для аутентификации с помощью ключа сервисной учётной записи (Service Account Key).

* `token` — токен для аутентификации. Значением свойства может быть содержимое токена или путь к файлу с токеном. {#token}

* `useMetadata` — использование режима аутентификации **Metadata**. Возможные значения: {#metadata}

    - `true` — использовать режим аутентификации **Metadata**;
    - `false` — не использовать режим аутентификации **Metadata**.

    Значение по умолчанию: `false`.

* `metadataURL` — эндпойнт для получения токена в режиме аутентификации **Metadata**.

* `localDatacenter` — название локального датацентра, в котором выполняется приложение.

* `secureConnection` — использование TLS. Возможные значения:

    - `true` — принудительно использовать TLS;
    - `false` — не использовать TLS.

    Обычно безопасное соединение указывается с помощью протокола `grpcs://` в строке JDBC URL. Небезопасное соединение указывается с помощью протокола `grpc://`.

* `secureConnectionCertificate` — сертификат CA для TLS-соединения. Значением свойства может быть содержимое сертификата или путь к файлу с сертификатом.

{% note info %}

Ссылки на файлы в свойствах `saFile`, `token` или `secureConnectionCertificate` должны содержать префикс `file:`. Примеры:

* `saFile=file:~/mysakey1.json`

* `token=file:/opt/secret/token-file`

* `secureConnectionCertificate=file:/etc/ssl/cacert.cer`

{% endnote %}

## Использование режима QueryService

По умолчанию JDBC-драйвер для {{ ydb-short-name }} использует старый API для обработки запросов, обеспечивая совместимость с предыдущими версиями {{ ydb-short-name }}. Однако у этого API есть [дополнительные ограничения](../../../concepts/limits-ydb.md#query). Чтобы отключить это поведение и использовать новый API QueryService, укажите свойство `useQueryService=true` в строке JDBC URL.

## Примеры JDBC URL {#jdbc-url}

{% include notitle [примеры](_includes/jdbc-url-examples.md) %}