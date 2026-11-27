# Cloud drives

On Linux and Windows, AirLink opens OneDrive, Dropbox and Google Drive accounts in tabs of their own. Connect them in **Ayarlar → Bulut sürücüleri** (Settings → Cloud drives). Each connected account gets a tab where you can:

- browse its folders and search the drive;
- download files, and upload them with **Yükle** (Upload) or by dragging them in;
- make folders, rename, and delete (to the drive's recycle bin);
- send a file straight to a paired device;
- see the used and free space.

The code is in `core/airlink-cloud` (Rust, the providers' web APIs), `core/airlink-capi/src/cloud.rs` (the `airlink_cloud_*` C calls) and `app/src/cloud_state.*` with the screens in `app/src/ui.cpp`.

## How signing in works

Signing in uses OAuth 2 with PKCE in the browser. AirLink opens the provider's sign-in page; once you allow access, the browser goes back to `http://localhost:47830/`, where AirLink waits for it.

- **The refresh token:** it goes to the system keyring: Secret Service on Linux (KWallet or GNOME Keyring), Credential Manager on Windows. Where no keyring answers, it goes to `<data dir>/cloud/tokens.json`, readable only by you.
- **The account list:** it is in `<data dir>/cloud/accounts.json`.
- **Removing an account:** this deletes both. The files in the drive stay as they are.

No AirLink server is involved: the computer talks to the providers directly.

## App registrations

Every provider signs in through an app registration. A build includes a provider only if it has that provider's registration. The registrations are looked up in this order:

1. `<data dir>/cloud/clients.json`, for using your own registration with any build:
   ```json
   {"onedrive": "<application id>", "dropbox": "<app key>",
    "gdrive": {"id": "<client id>", "secret": "<client secret>"}}
   ```
2. The environment at run time.
3. The environment at build time.
4. AirLink's own registration, built in. Dropbox has one so far.

The environment variables are `AIRLINK_ONEDRIVE_CLIENT_ID`, `AIRLINK_DROPBOX_CLIENT_ID`, `AIRLINK_GDRIVE_CLIENT_ID` and `AIRLINK_GDRIVE_CLIENT_SECRET`.

The release workflow builds with the repository variables of the same names, and with the secret `AIRLINK_GDRIVE_CLIENT_SECRET`. They are needed only for a provider without a built-in registration, or to replace it.

The registrations are public clients: their IDs are not secrets. Google's desktop "client secret" is not confidential either, by Google's own rules for installed apps.

Every provider gets the same redirect address: `http://localhost:47830/`.

### OneDrive (Microsoft Entra)

You need a Microsoft Entra tenant. A free Azure account comes with one.

1. In the [Microsoft Entra admin center](https://entra.microsoft.com), go to **App registrations → New registration**.
2. Fill in the registration:
   - **Name:** AirLink
   - **Supported account types:** *Accounts in any organizational directory and personal Microsoft accounts*
   - **Redirect URI:** platform *Public client/native (mobile & desktop)*, address `http://localhost:47830/`
3. Under **API permissions**, add the Microsoft Graph delegated permissions `Files.ReadWrite`, `User.Read` and `offline_access`.
4. The **Application (client) ID** on the overview page is `AIRLINK_ONEDRIVE_CLIENT_ID`.

### Dropbox

1. In the [App Console](https://www.dropbox.com/developers/apps), choose **Create app**.
2. Pick **Scoped access**, then **Full Dropbox**, and give it a name. The name must be unique across Dropbox.
3. On the **Settings** tab:
   - Add the redirect URI `http://localhost:47830/`. Dropbox matches it exactly.
   - Set **Allow public clients (Implicit Grant & PKCE)** to *Allow*.
4. On the **Permissions** tab, tick these and **Submit**:
   - `account_info.read`
   - `files.metadata.read` and `files.metadata.write`
   - `files.content.read` and `files.content.write`
5. The **App key** is `AIRLINK_DROPBOX_CLIENT_ID`.

A new app is in *development* status, which allows up to 500 users. Above that, Dropbox reviews it for *production*.

### Google Drive

1. In the [Google Cloud console](https://console.cloud.google.com), make a project. Under **APIs & Services → Library**, enable the **Google Drive API**.
2. Under **Google Auth Platform**, set up the consent screen:
   - **Branding:** the app's name and a support address.
   - **Audience:** *External*. Add your own Google account as a test user.
   - **Data access:** add the scope `https://www.googleapis.com/auth/drive`.
3. Under **Clients → Create client**, choose *Desktop app*. Its ID and secret are `AIRLINK_GDRIVE_CLIENT_ID` and `AIRLINK_GDRIVE_CLIENT_SECRET`.

Google's limits for an unverified app:

- **Restricted scope:** the full-Drive scope is restricted.
- **In testing:** only its test users (up to 100) can sign in, and their sign-ins expire after 7 days, so AirLink asks them to connect again.
- **In production but unverified:** Google shows a warning screen, and at most 100 users can sign in.
- **For everyone:** Google's verification and a yearly CASA security assessment are needed.

## Limits

- Only files are uploaded; a dropped folder is skipped with a message.
- Google's own documents are downloaded as Office files (Docs → .docx, Sheets → .xlsx, Slides → .pptx, Drawings → .png); forms and sites can't be downloaded.
- Opening a folder found by search starts from the drive's root in the folder path.
- Android has no drive tabs: there, the providers' own apps reach their drives.
