# MateriAI - AI-Powered Material Generator for Unreal Engine

## Overview

MateriAI is an Unreal Engine 5 editor plugin that generates PBR (Physically Based Rendering) materials using AI. Users describe the material they want in plain text (and optionally provide a reference image), and the plugin communicates with the MateriAI cloud backend to generate a complete texture set — Base Color, Normal Map, and Roughness Map — which can then be saved as native Unreal material assets directly within the editor.

## Requirements

- **Unreal Engine 5.0+**
- An active MateriAI account (register at [matgenai.com](https://matgenai.com))
- Internet connection (required for API communication with the generation backend)

## Installation

1. Copy the entire `MateriAI` plugin folder into your project's `Plugins/` directory (e.g., `MyProject/Plugins/MateriAI/`).
2. Restart the Unreal Editor. The plugin will be automatically detected.
3. If needed, enable the plugin via **Edit > Plugins**, search for "MateriAI", and check the **Enabled** box.
4. A **MateriAI** button will appear in the editor toolbar, and the plugin will also be accessible from **Window > MateriAI** in the main menu.

---

## Plugin Architecture

### Module Structure

The plugin is a single editor module (`FMateriAIModule`) of type `Editor`, loaded during the `Default` loading phase. It is composed of the following source files:

| File | Description |
|------|-------------|
| `Public/MateriAI.h` | Main module header. Declares `FMateriAIModule` which implements `IModuleInterface`. Contains all API endpoint configuration, UI state variables, authentication state, and method declarations for generation, login, saving, and HTTP response handling. |
| `Private/MateriAI.cpp` | Main module implementation (~1300 lines). Implements plugin startup/shutdown lifecycle, Slate UI construction, HTTP request/response logic for authentication and generation, ZIP extraction pipeline, texture creation from raw image data, material asset creation and saving. |
| `Public/MateriAILogin.h` | Declares `SMateriAILoginWidget`, a Slate compound widget for the login screen. Uses Slate delegates (`FOnLoginRequest`, `FSimpleDelegate`) to communicate login events back to the main module. |
| `Private/MateriAILogin.cpp` | Implements the login widget UI (email/password fields, sign-in button, "Register" link) and input validation. |
| `Public/MateriAICommands.h` | Declares `FMateriAICommands`, a `TCommands<>` subclass that registers the toolbar button command (`OpenPluginWindow`). |
| `Private/MateriAICommands.cpp` | Registers the `UI_COMMAND` for the plugin window button. |
| `Public/MateriAIStyle.h` | Declares `FMateriAIStyle`, managing the Slate style set for the plugin (toolbar icon, etc.). |
| `Private/MateriAIStyle.cpp` | Implements style initialization, loading resources from the plugin's `Resources/` folder, and registering the style with `FSlateStyleRegistry`. |

### Key Classes

- **`FMateriAIModule`** — The core module class. Manages the entire plugin lifecycle, UI, networking, and asset pipeline. Registered as a Nomad Tab Spawner so the plugin window can be opened and docked anywhere in the editor.
- **`SMateriAILoginWidget`** — A self-contained Slate widget handling the login screen. Communicates with `FMateriAIModule` via two delegates: `OnLoginRequest` (triggers the HTTP login call) and `OnLoginSuccess` (notifies the module to switch to the generation UI).
- **`FMateriAICommands`** — Registers the toolbar button that opens the plugin window.
- **`FMateriAIStyle`** — Manages the plugin's Slate style set including the toolbar icon (`PlaceholderButtonIcon.svg` from the `Resources/` folder).

---

## Implementation Details

### Startup & Registration

When the module starts up (`StartupModule`), it:
1. Initializes the Slate style (`FMateriAIStyle::Initialize()`) and reloads textures.
2. Registers toolbar commands (`FMateriAICommands::Register()`).
3. Populates model options (Basic, Advanced, Professional) and preview shape options (Sphere, Cube), storing model-to-API-tier mappings in a `TMap`.
4. Maps the toolbar button action to `PluginButtonClicked()`.
5. Registers a startup callback with `UToolMenus` to add the plugin to the Window menu.
6. Registers a **Nomad Tab Spawner** (`MateriAI` tab) so the plugin panel can be opened as a dockable editor tab.

On shutdown (`ShutdownModule`), all registrations are reversed: callbacks unregistered, styles shut down, commands unregistered, and any open tab is closed.

### User Interface

The plugin UI is built entirely with **Slate** (no UMG/Blueprints). It uses an `SWidgetSwitcher` with two slots:

- **Slot 0 — Login Screen** (`SMateriAILoginWidget`): Email field, password field (masked), status text, Login button (disabled until both fields are non-empty), and a "Register" link that opens the registration website in the system browser via `FPlatformProcess::LaunchURL`.

- **Slot 1 — Generation Screen**: Displayed when `bIsUserLoggedIn == true`. Contains:
  - **Credits display** — Shows remaining credits, updated after login and after each generation.
  - **Logout button** — Clears auth token, resets login state, switches back to login screen.
  - **Preview image** — 256×256 `SImage` widget bound to `PreviewBrush`, which updates dynamically when generation completes.
  - **Model selector** — `SComboBox` populated with model options (Basic/Advanced/Professional), each mapped to an API tier string via `ModelIdMap`.
  - **Prompt input** — `SMultiLineEditableTextBox` for entering the text description.
  - **Reference image picker** — Opens a native file dialog (`IDesktopPlatform::OpenFileDialog`) filtering for PNG/JPG files.
  - **Seamless checkbox** — Toggles seamless tiling mode (adds 50 credits to cost).
  - **Generate button** — Triggers the generation HTTP request.
  - **Save button** — Visible only after a successful generation (`LastBaseColorTexture.IsValid()`). Saves the generated textures and material as Unreal assets.

The widget switcher automatically switches between login and generation screens based on a lambda checking `bIsUserLoggedIn`.

### Authentication Flow

1. User enters email and password in `SMateriAILoginWidget` and clicks Login.
2. The widget fires the `OnLoginRequest` delegate, which calls `FMateriAIModule::SendLoginRequest()`.
3. A `POST` request is sent to `{API_BASE_URL}/auth/login` with a JSON body containing `email` and `password`.
4. On receiving the response (`OnLoginResponseReceived`):
   - **200 OK**: The module parses the JSON for an `access_token` or `token` field, stores it in `UserAuthToken`, sets `bIsUserLoggedIn = true`, triggers `OnLoginSuccess()` which also calls `FetchUserCredits()`.
   - **Error**: The error message is parsed from the response JSON (`message` or `error` field) and displayed in the login widget's status text block via `OnLoginFailed()`.
5. All subsequent API requests include the `Authorization: Bearer <token>` header.

### Material Generation Pipeline

The generation pipeline is the core of the plugin and involves multiple stages:

#### 1. Request Construction
When the user clicks Generate, `OnGenerateClicked()` reads the prompt text and calls `SendPromptToServer()`. The method:
- Determines the API tier from `ModelIdMap` based on the selected model.
- Chooses the endpoint based on whether a reference image was selected:
  - With image: `POST {API_GENERATE_BASE_URL}/generate/generate-zip-from-image`
  - Text only: `POST {API_GENERATE_BASE_URL}/generate/generate-zip-from-text`
- For **image requests**: Constructs a `multipart/form-data` payload with fields for `tier`, `seamless`, `prompt`, and the `image` binary data. The image file is read from disk via `FFileHelper::LoadFileToArray` and appended to the multipart body with proper MIME boundaries.
- For **text requests**: Sends a JSON body with `prompt`, `tier`, and `seamless` fields.
- Attaches the Bearer auth token header.

#### 2. Response Handling & Retry Logic
The response handler (`OnResponseReceived`) implements automatic retry logic:
- On success (HTTP 2xx): Resets retry count, clears `bIsGenerating`, passes response data to `HandleMaterialZipResponse()`, and refreshes user credits.
- On failure: Increments `RetryCount` and re-sends the same request up to `MaxRetryAttempts` (3). After exhausting retries, resets state and logs an error.

#### 3. ZIP Download & Extraction (`HandleMaterialZipResponse`)
The API response contains a JSON body with a `download_url` field pointing to the generated ZIP file. The method:
1. Parses the JSON response to extract the download URL.
2. Issues a `GET` request to download the ZIP file.
3. Validates the ZIP by checking for the `PK` magic header bytes.
4. Saves the ZIP to `{ProjectSavedDir}/GeneratedMaterial.zip`.
5. Clears and recreates the extraction directory `{ProjectSavedDir}/GeneratedMaterial/`.
6. Extracts the ZIP using platform-specific commands:
   - **Windows**: `powershell -Command "Expand-Archive -Force ..."`
   - **Linux/Mac**: `unzip -o ...`
   Executed via `FPlatformProcess::ExecProcess`.
7. Searches for `base_texture.png`, `normal_map.png`, and `roughness_map.png` in the extracted files. Handles nested directory structures (common with `Expand-Archive`) by recursively searching all extracted files.

#### 4. Texture Creation (Game Thread)
Since `UTexture2D` creation must happen on the Game Thread, an `AsyncTask(ENamedThreads::GameThread, ...)` is dispatched:
1. Raw PNG/JPEG bytes are passed to a lambda that auto-detects the image format by inspecting magic header bytes (PNG: `89 50 4E 47`, JPEG: `FF D8`, RIFF/WebP, etc.).
2. The `IImageWrapperModule` is used to decompress the image data, trying multiple formats as fallbacks (PNG → JPEG → BMP → EXR).
3. A transient `UTexture2D` is created (`UTexture2D::CreateTransient`) with `PF_B8G8R8A8` pixel format.
4. Raw BGRA pixel data is copied into the texture's mip bulk data and `UpdateResource()` is called.
5. The generated textures are stored in `TStrongObjectPtr` members (`LastBaseColorTexture`, `LastNormalTexture`, `LastRoughnessTexture`) to prevent garbage collection.
6. The preview brush is updated with the Base Color texture for display in the UI.

### Saving Materials as Unreal Assets

When the user clicks Save (`OnSave3DClicked`):

1. **Export textures to PNG**: Each generated texture's mip data is locked, read, compressed to PNG via `IImageWrapper`, and saved to a timestamped folder under `{ProjectSavedDir}/GeneratedMaterials/{timestamp}/`.

2. **Import as UTexture2D assets**: The saved PNGs are imported into the project's Content directory (`/Game/GeneratedMaterials/{timestamp}/`) using `UAutomatedAssetImportData` via the `AssetTools` module.

3. **Save texture .uasset files**: Each imported texture is moved into its own UPackage, flagged as `RF_Public | RF_Standalone`, the package is marked dirty, registered with the Asset Registry, and saved to disk via `UPackage::SavePackage`.

4. **Create Material asset**: A new `UMaterial` is created using `UMaterialFactoryNew`, then:
   - A `UMaterialExpressionTextureSample` node is created for the **Base Color** texture and connected to `MP_BaseColor`.
   - A `UMaterialExpressionTextureSample` node is created for the **Normal** texture (with `TC_Normalmap` compression, `SRGB = false`, `SAMPLERTYPE_Normal`) and connected to `MP_Normal`.
   - A `UMaterialExpressionTextureSample` node is created for the **Roughness** texture and connected to `MP_Roughness`.
   - All material expression connections are made via `UMaterialEditingLibrary::ConnectMaterialProperty`.
   - The material is post-edit-changed, marked dirty, registered with the Asset Registry, and saved as a `.uasset`.

### Credit System

- After login and after each successful generation, `FetchUserCredits()` sends a `GET` request to `{API_BASE_URL}/users/me/credits`.
- The response JSON structure is `{ "data": { "credits": <int> } }`.
- The credit balance is displayed in the UI header and updates automatically.
- Different model tiers consume different credit amounts: Basic (15), Advanced (20), Professional (50). Seamless mode adds 50 credits to the cost.

### Utility Features

- **`LoadTextureFromFile`**: A helper method that reads an image file from disk, auto-detects the format (PNG/JPEG/BMP), decompresses it, and creates a transient `UTexture2D`. Used for loading textures from the extraction directory.
- **`CreateMaterial`**: Creates a `UMaterialInstanceDynamic` from a base material (`/MateriAI/M_BaseMaterial`) and assigns the Base Color, Normal, and Roughness texture parameters. Used for dynamic preview materials.

---

## API Endpoints

The plugin communicates with two backend services:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `{API_BASE_URL}/auth/login` | POST | Authenticates the user with email/password. Returns an access token. |
| `{API_BASE_URL}/users/me/credits` | GET | Fetches the authenticated user's remaining credit balance. |
| `{API_GENERATE_BASE_URL}/generate/generate-zip-from-text` | POST | Generates a material texture set from a text prompt. Returns a ZIP download URL. |
| `{API_GENERATE_BASE_URL}/generate/generate-zip-from-image` | POST | Generates a material texture set from a text prompt + reference image. Returns a ZIP download URL. |

All authenticated requests include an `Authorization: Bearer <token>` header.

---

## Generated Output Format

The generation API produces a ZIP archive containing three PNG texture maps:
- `base_texture.png` — The albedo/base color map.
- `normal_map.png` — The tangent-space normal map.
- `roughness_map.png` — The roughness map.

These are extracted locally, converted to `UTexture2D` objects, and can be saved as standard Unreal `.uasset` files.

---

## Supported Platforms

- **Windows** (Win64)
- **macOS** (Mac)
- **Linux**

ZIP extraction uses platform-specific commands: PowerShell `Expand-Archive` on Windows, and `unzip` on Linux/macOS.

---

## User Workflow Summary

1. **Open** the MateriAI panel via the toolbar button or **Window > MateriAI**.
2. **Log in** with your MateriAI account credentials.
3. **Select a model** tier (Basic, Advanced, or Professional).
4. **Enter a prompt** describing the desired material (e.g., "weathered red brick with moss growth").
5. *(Optional)* **Pick a reference image** to guide generation.
6. *(Optional)* **Enable Seamless** for tileable textures.
7. Click **Generate** and wait for the AI to produce the texture set.
8. **Preview** the result on a sphere/cube in the preview panel.
9. Click **Save** to import the textures and create a ready-to-use Unreal Material asset in your project's Content directory.

---

## Support

For questions, bug reports, or feature requests, contact us at matgenai.office@gmail.com or visit [matgenai.com](https://matgenai.com).

## License

Copyright MateriAI 2026. All Rights Reserved.
