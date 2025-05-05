// Copyright Epic Games, Inc. All Rights Reserved.

#include "MateriAI.h"
#include "MateriAIStyle.h"
#include "MateriAICommands.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "CoreMinimal.h"
#include "AssetToolsModule.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "UObject/Package.h"
#include "Misc/DateTime.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectMacros.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "MaterialEditingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Factories/MaterialFactoryNew.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"


static const FName MateriAITabName("MateriAI");

#define LOCTEXT_NAMESPACE "FMateriAIModule"

TArray<uint8> LastRawImageData;
UTexture2D* LastBaseColorTexture = nullptr;
UTexture2D* LastNormalTexture = nullptr;
UTexture2D* LastRoughnessTexture = nullptr;

int32 LastImageWidth = 0;
int32 LastImageHeight = 0;

int32 RetryCount = 0;
constexpr int32 MaxRetryAttempts = 3;

void FMateriAIModule::StartupModule()
{
	FMateriAIStyle::Initialize();
	FMateriAIStyle::ReloadTextures();
	FMateriAICommands::Register();

	PreviewShapeOptions.Empty();
	TSharedPtr<FString> SphereOption = MakeShared<FString>(TEXT("Sphere"));
	TSharedPtr<FString> CubeOption = MakeShared<FString>(TEXT("Cube"));
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FMateriAICommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FMateriAIModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMateriAIModule::RegisterMenus));
	
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(MateriAITabName, FOnSpawnTab::CreateRaw(this, &FMateriAIModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("FMateriAITabTitle", "MateriAI"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FMateriAIModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FMateriAIStyle::Shutdown();
	FMateriAICommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MateriAITabName);

	if (TSharedPtr<SDockTab> ExistingTab = FGlobalTabmanager::Get()->FindExistingLiveTab(MateriAITabName))
	{
		ExistingTab->RequestCloseTab();
	}
}



TSharedRef<SDockTab> FMateriAIModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
	.TabRole(ETabRole::NomadTab)
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(8)
		[
			SNew(SBox)
			.WidthOverride(256)
			.HeightOverride(256)
			[
				SNew(SOverlay)

				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(256)
					.HeightOverride(256)
					[
						SNew(SImage)
						.Image_Lambda([this]() -> const FSlateBrush*
						{
							return PreviewBrush.IsValid() ? PreviewBrush.Get() : nullptr;
						})
					]
				]
			]
		]


		+ SVerticalBox::Slot()
		.AutoHeight().Padding(8, 8, 8, 8)
		[
			SNew(STextBlock).Text(FText::FromString("Prompt:"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(2)
		[
			SNew(SBox)
			.MinDesiredHeight(60.f)
			[
				SAssignNew(PromptTextBox, SMultiLineEditableTextBox)
				.HintText(FText::FromString("Describe the material..."))
				.AutoWrapText(true)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(8)
		[
			SNew(SButton)
			.Text(FText::FromString("Choose Reference Image (Optional)"))
			.OnClicked_Raw(this, &FMateriAIModule::OnPickImageClicked)
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 0, 8, 2))
		[
			SNew(STextBlock)
			.Text_Lambda([this]() {
				return !SelectedImagePath.IsEmpty()
					? FText::FromString("Selected Image: " + FPaths::GetCleanFilename(SelectedImagePath))
					: FText::FromString("No reference image selected");
			})
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 8, 8, 2))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() {
				return bOnlyGenerateBaseTexture ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) {
				bOnlyGenerateBaseTexture = (NewState == ECheckBoxState::Checked);
				if (bOnlyGenerateBaseTexture)
					LastBaseColorTexture = nullptr;
				else
					LastImageWidth = 0;
			})
			.Content()
			[
				SNew(STextBlock)
				.Text(FText::FromString("Generate Only Base Texture (2D)"))
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 6, 8, 4))
		[
			SNew(STextBlock)
			.Text_Lambda([this]() {
				return bIsGenerating ? FText::FromString("Generating...") : FText::GetEmpty();
			})
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 0, 8, 8))
		[
			SNew(SButton)
			.Text(FText::FromString("Generate"))
			.OnClicked_Raw(this, &FMateriAIModule::OnGenerateClicked)
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 0, 2, 4))
		[
			SNew(SButton)
			.Text(FText::FromString("Save"))
			.IsEnabled_Lambda([this]() { return !bIsSaving; })
			.Visibility_Lambda([this]() {
				return (bOnlyGenerateBaseTexture && LastRawImageData.Num() > 0 && LastImageWidth > 0 && LastImageHeight > 0)
					? EVisibility::Visible
					: EVisibility::Collapsed;
			})
			.OnClicked_Raw(this, &FMateriAIModule::OnSaveClicked)
		]

		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 0, 2, 4))
		[
			SNew(SButton)
			.Text(FText::FromString("Save"))
			.IsEnabled_Lambda([this]() { return !bIsSaving; })
			.Visibility_Lambda([this]() {
				return (!bOnlyGenerateBaseTexture && LastBaseColorTexture)
					? EVisibility::Visible
					: EVisibility::Collapsed;
			})
			.OnClicked_Raw(this, &FMateriAIModule::OnSave3DClicked)
		]


		+ SVerticalBox::Slot()
		.AutoHeight().Padding(FMargin(8, 0, 2, 2))
		[
			SNew(STextBlock)
			.Text_Lambda([this]() {
				return bIsSaving ? FText::FromString("Saving...") : FText::GetEmpty();
			})
		]
	];
}

FReply FMateriAIModule::OnSaveClicked()
{
	if (LastRawImageData.Num() == 0 || LastImageWidth == 0 || LastImageHeight == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("❌ No texture data available to save."));
		return FReply::Handled();
	}

	bIsSaving = true;

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> PngImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (!PngImageWrapper.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Failed to create PNG image wrapper."));
		bIsSaving = false;
		return FReply::Handled();
	}

	PngImageWrapper->SetRaw(LastRawImageData.GetData(), LastRawImageData.Num(), LastImageWidth, LastImageHeight, ERGBFormat::BGRA, 8);
	const TArray64<uint8>& CompressedData = PngImageWrapper->GetCompressed();

	FString SaveDir = FPaths::ProjectContentDir() / TEXT("SavedTextures/");
	IFileManager::Get().MakeDirectory(*SaveDir, true);

	FString FileName = "GeneratedTexture_" + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")) + ".png";
	FString FullPath = SaveDir / FileName;

	if (FFileHelper::SaveArrayToFile(CompressedData, *FullPath))
	{
		UE_LOG(LogTemp, Log, TEXT("✅ Saved PNG to %s"), *FullPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Failed to save PNG."));
	}

	bIsSaving = false;
	return FReply::Handled();
}

UTexture2D* FMateriAIModule::ImportTextureAsset(const FString& FilePath, const FString& DestinationPath)
{
	FAssetToolsModule& AssetToolsModule = FAssetToolsModule::GetModule();
    
	TArray<FString> FilesToImport;
	FilesToImport.Add(FilePath);

	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->DestinationPath = DestinationPath;
	ImportData->Filenames = FilesToImport;
	ImportData->bReplaceExisting = true;

	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);
	if (ImportedAssets.Num() > 0)
	{
		return Cast<UTexture2D>(ImportedAssets[0]);
	}
	return nullptr;
}


FReply FMateriAIModule::OnSave3DClicked()
{
    if (!LastBaseColorTexture)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ No 3D material to save."));
        return FReply::Handled();
    }

    bIsSaving = true;

    FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    FString BaseFolder = FPaths::ProjectSavedDir() / TEXT("GeneratedMaterials/") / Timestamp;
    IFileManager::Get().MakeDirectory(*BaseFolder, true);

    // Save texture PNGs to disk
    auto SaveTextureAsPNG = [](UTexture2D* Texture, const FString& Path)
    {
        if (!Texture) return;

        FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
        void* Data = Mip.BulkData.Lock(LOCK_READ_ONLY);

        TArray<uint8> RawData;
        RawData.AddUninitialized(Mip.BulkData.GetBulkDataSize());
        FMemory::Memcpy(RawData.GetData(), Data, RawData.Num());

        Mip.BulkData.Unlock();

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

        ImageWrapper->SetRaw(RawData.GetData(), RawData.Num(), Texture->GetSizeX(), Texture->GetSizeY(), ERGBFormat::BGRA, 8);
        const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed();

        FFileHelper::SaveArrayToFile(CompressedData, *Path);
    };

    SaveTextureAsPNG(LastBaseColorTexture, BaseFolder / TEXT("BaseColor.png"));
    if (LastNormalTexture) SaveTextureAsPNG(LastNormalTexture, BaseFolder / TEXT("Normal.png"));
    if (LastRoughnessTexture) SaveTextureAsPNG(LastRoughnessTexture, BaseFolder / TEXT("Roughness.png"));

    FString DestinationPath = FString(TEXT("/Game/GeneratedMaterials/")) + Timestamp;

    // Import PNGs as UTexture2D
    UTexture2D* ImportedBaseColor = ImportTextureAsset(BaseFolder / TEXT("BaseColor.png"), DestinationPath);
    UTexture2D* ImportedNormal = nullptr;
    UTexture2D* ImportedRoughness = nullptr;

    if (FPaths::FileExists(BaseFolder / TEXT("Normal.png")))
    {
        ImportedNormal = ImportTextureAsset(BaseFolder / TEXT("Normal.png"), DestinationPath);
    }
    if (FPaths::FileExists(BaseFolder / TEXT("Roughness.png")))
    {
        ImportedRoughness = ImportTextureAsset(BaseFolder / TEXT("Roughness.png"), DestinationPath);
    }

    // Save textures as .uasset files
    auto SaveTextureAsset = [](UTexture2D* Texture, const FString& AssetPath, const FString& AssetName) -> UTexture2D*
    {
        if (!Texture) return nullptr;

        FString FullPackageName = AssetPath + TEXT("/") + AssetName;
        FString UniquePackageName, UniqueAssetName;
        FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(FullPackageName, TEXT(""), UniquePackageName, UniqueAssetName);

        UPackage* TexturePackage = CreatePackage(*UniquePackageName);
        Texture->Rename(*UniqueAssetName, TexturePackage, REN_DontCreateRedirectors | REN_NonTransactional);
        Texture->SetFlags(RF_Public | RF_Standalone);
        TexturePackage->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Texture);

        FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());
        if (UPackage::SavePackage(TexturePackage, Texture, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone, *PackageFileName))
        {
            UE_LOG(LogTemp, Log, TEXT("✅ Saved texture asset: %s"), *PackageFileName);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("❌ Failed to save texture asset: %s"), *PackageFileName);
        }

        return Texture;
    };

    ImportedBaseColor = SaveTextureAsset(ImportedBaseColor, DestinationPath, TEXT("BaseColor"));
    if (ImportedNormal) ImportedNormal = SaveTextureAsset(ImportedNormal, DestinationPath, TEXT("Normal"));
    if (ImportedRoughness) ImportedRoughness = SaveTextureAsset(ImportedRoughness, DestinationPath, TEXT("Roughness"));

    // Create and save material asset
    FString MaterialPackageName = DestinationPath + TEXT("/GeneratedMaterial");
    FString MaterialAssetName = TEXT("GeneratedMaterial");

    FAssetToolsModule& AssetToolsModule = FAssetToolsModule::GetModule();
    AssetToolsModule.Get().CreateUniqueAssetName(MaterialPackageName, TEXT(""), MaterialPackageName, MaterialAssetName);

    UPackage* Package = CreatePackage(*MaterialPackageName);
    if (!Package)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Failed to create material package."));
        bIsSaving = false;
        return FReply::Handled();
    }

    UMaterialFactoryNew* MaterialFactory = NewObject<UMaterialFactoryNew>();
    UObject* NewAsset = MaterialFactory->FactoryCreateNew(
        UMaterial::StaticClass(), Package, *MaterialAssetName, RF_Standalone | RF_Public, nullptr, GWarn
    );

    UMaterial* NewMaterial = Cast<UMaterial>(NewAsset);
    if (!NewMaterial)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Failed to create material asset."));
        bIsSaving = false;
        return FReply::Handled();
    }

    if (ImportedBaseColor)
    {
        UMaterialExpressionTextureSample* BaseColorSample = Cast<UMaterialExpressionTextureSample>(
            UMaterialEditingLibrary::CreateMaterialExpression(NewMaterial, UMaterialExpressionTextureSample::StaticClass())
        );
        BaseColorSample->Texture = ImportedBaseColor;
        UMaterialEditingLibrary::ConnectMaterialProperty(BaseColorSample, TEXT("RGB"), MP_BaseColor);
    }

    if (ImportedNormal)
    {
    	ImportedNormal->CompressionSettings = TC_Normalmap;
    	ImportedNormal->SRGB = false;
    	ImportedNormal->PostEditChange();
    	
        UMaterialExpressionTextureSample* NormalSample = Cast<UMaterialExpressionTextureSample>(
            UMaterialEditingLibrary::CreateMaterialExpression(NewMaterial, UMaterialExpressionTextureSample::StaticClass())
        );
        NormalSample->Texture = ImportedNormal;
        NormalSample->SamplerType = SAMPLERTYPE_Normal;
        UMaterialEditingLibrary::ConnectMaterialProperty(NormalSample, TEXT("RGB"), MP_Normal);
    }

    if (ImportedRoughness)
    {
        UMaterialExpressionTextureSample* RoughnessSample = Cast<UMaterialExpressionTextureSample>(
            UMaterialEditingLibrary::CreateMaterialExpression(NewMaterial, UMaterialExpressionTextureSample::StaticClass())
        );
        RoughnessSample->Texture = ImportedRoughness;
        UMaterialEditingLibrary::ConnectMaterialProperty(RoughnessSample, TEXT("RGB"), MP_Roughness);
    }

    NewMaterial->PostEditChange();
    NewMaterial->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewMaterial);

    FString PackageFileName = FPackageName::LongPackageNameToFilename(MaterialPackageName, FPackageName::GetAssetPackageExtension());
    bool bSaved = UPackage::SavePackage(Package, NewMaterial, EObjectFlags::RF_Public | RF_Standalone, *PackageFileName);

    if (bSaved)
    {
        UE_LOG(LogTemp, Log, TEXT("✅ Successfully saved generated material to: %s"), *PackageFileName);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Failed to save generated material!"));
    }

    bIsSaving = false;
    return FReply::Handled();
}

FReply FMateriAIModule::OnPickImageClicked()
{
	TArray<FString> OutFiles;
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

	if (DesktopPlatform)
	{
		const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		DesktopPlatform->OpenFileDialog(
			ParentWindowHandle,
			TEXT("Choose Image"),
			TEXT(""),
			TEXT(""),
			TEXT("Image Files (*.png;*.jpg)|*.png;*.jpg"),
			EFileDialogFlags::None,
			OutFiles
		);

		if (OutFiles.Num() > 0)
		{
			SelectedImagePath = OutFiles[0];
			UE_LOG(LogTemp, Log, TEXT("✅ Image selected for POST: %s"), *SelectedImagePath);
		}
	}

	return FReply::Handled();
}


FReply FMateriAIModule::OnGenerateClicked()
{
	if (PromptTextBox.IsValid())
	{
		const FString Prompt = PromptTextBox->GetText().ToString();
		UE_LOG(LogTemp, Warning, TEXT("Prompt: %s"), *Prompt);

		bIsGenerating = true; // update status UI
		SendPromptToServer(Prompt); // 🚀 ADDED
	}

	return FReply::Handled();
}

void FMateriAIModule::SendPromptToServer(const FString& Prompt)
{
	if (RetryCount == 0)
	{
		bIsGenerating = true;
	}

	const bool bHasImage = !SelectedImagePath.IsEmpty();
	const bool bJustBase = bOnlyGenerateBaseTexture;

	FString Endpoint;

	if (bJustBase && bHasImage)
	{
		Endpoint = TEXT("https://nn74i2a85m.execute-api.us-east-1.amazonaws.com/prod/api/v1/generate/generate-base-image-with-image");
	}
	else if (bJustBase && !bHasImage)
	{
		Endpoint = TEXT("https://nn74i2a85m.execute-api.us-east-1.amazonaws.com/prod/api/v1/generate/generate-base-image");
	}
	else if (!bJustBase && bHasImage)
	{
		Endpoint = TEXT("https://nn74i2a85m.execute-api.us-east-1.amazonaws.com/prod/api/v1/generate/generate-zip-from-image");
	}
	else
	{
		Endpoint = TEXT("https://nn74i2a85m.execute-api.us-east-1.amazonaws.com/prod/api/v1/generate/generate-zip-from-text");
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Endpoint);
	Request->SetVerb("POST");

	if (bHasImage)
	{
		Request->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=MateriAIBoundary"));

		TArray<uint8> ImageBytes;
		if (FFileHelper::LoadFileToArray(ImageBytes, *SelectedImagePath))
		{
			FString Payload;
			FString Boundary = TEXT("MateriAIBoundary");

			Payload += "--" + Boundary + "\r\n";
			Payload += "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n";
			Payload += Prompt + "\r\n";

			Payload += "--" + Boundary + "\r\n";
			Payload += "Content-Disposition: form-data; name=\"image\"; filename=\"input.png\"\r\n";
			Payload += "Content-Type: image/png\r\n\r\n";

			TArray<uint8> FinalPayload;
			FTCHARToUTF8 Converter(*Payload);
			FinalPayload.Append((uint8*)Converter.Get(), Converter.Length());

			FinalPayload.Append(ImageBytes);

			FString End = TEXT("\r\n--") + Boundary + TEXT("--\r\n");
			FTCHARToUTF8 EndConv(*End);
			FinalPayload.Append((uint8*)EndConv.Get(), EndConv.Length());

			Request->SetContent(FinalPayload);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("❌ Failed to read image file."));
			bIsGenerating = false;
			return;
		}
	}
	else
	{
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
		JsonObject->SetStringField("prompt", Prompt);

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

		Request->SetContentAsString(OutputString);
	}

	Request->OnProcessRequestComplete().BindLambda([this, Prompt](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		this->OnResponseReceived(Req, Resp, bSuccess, Prompt);
	});

	Request->ProcessRequest();
}

void FMateriAIModule::HandleBaseTextureResponse(const TArray<uint8>& JsonResponseData)
{
	UE_LOG(LogTemp, Log, TEXT("✅ Received JSON Response (%d bytes)"), JsonResponseData.Num());

	FString JsonString;
	FFileHelper::BufferToString(JsonString, JsonResponseData.GetData(), JsonResponseData.Num());

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Failed to parse JSON response."));
		return;
	}

	FString Base64String;
	if (!JsonObject->TryGetStringField(TEXT("body"), Base64String))
	{
		UE_LOG(LogTemp, Error, TEXT("❌ 'body' field not found in JSON response."));
		return;
	}

	TArray<uint8> ImageData;
	if (!FBase64::Decode(Base64String, ImageData))
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Failed to decode base64 PNG image data."));
		return;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (ImageWrapper->SetCompressed(ImageData.GetData(), ImageData.Num()))
	{
		TArray<uint8> UncompressedBGRA;
		if (ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, UncompressedBGRA))
		{
			LastRawImageData = UncompressedBGRA;
			LastImageWidth = ImageWrapper->GetWidth();
			LastImageHeight = ImageWrapper->GetHeight();

			UTexture2D* GeneratedTexture = UTexture2D::CreateTransient(
				ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), PF_B8G8R8A8);

			if (GeneratedTexture)
			{
				void* TextureData = GeneratedTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
				FMemory::Memcpy(TextureData, UncompressedBGRA.GetData(), UncompressedBGRA.Num());
				GeneratedTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
				GeneratedTexture->UpdateResource();

				PreviewTexture = GeneratedTexture;

				FSlateBrush* NewBrush = new FSlateBrush();
				NewBrush->SetResourceObject(PreviewTexture);
				NewBrush->ImageSize = FVector2D(256, 256);
				PreviewBrush = MakeShareable(NewBrush);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("❌ Failed to decompress image."));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Failed to decode PNG image format."));
	}
}

void FMateriAIModule::HandleMaterialZipResponse(const TArray<uint8>& ResponseData)
{
	UE_LOG(LogTemp, Log, TEXT("✅ Received server response (%d bytes)"), ResponseData.Num());

	// Step 1: Parse JSON
	FString ResponseString;
	FFileHelper::BufferToString(ResponseString, ResponseData.GetData(), ResponseData.Num());

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Failed to parse JSON response."));
		return;
	}

	FString DownloadUrl;
	if (!JsonObject->TryGetStringField(TEXT("download_url"), DownloadUrl))
	{
		UE_LOG(LogTemp, Error, TEXT("❌ 'download_url' field not found in response."));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("🔵 Download URL: %s"), *DownloadUrl);

	// Step 2: Download the ZIP file
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(DownloadUrl);
	HttpRequest->SetVerb(TEXT("GET"));
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			if (!bWasSuccessful || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				UE_LOG(LogTemp, Error, TEXT("❌ Failed to download ZIP file. Status: %d"), Response.IsValid() ? Response->GetResponseCode() : -1);
				return;
			}

			TArray<uint8> ZipData = Response->GetContent();
			UE_LOG(LogTemp, Log, TEXT("✅ Downloaded ZIP (%d bytes)"), ZipData.Num());

			// Step 3: Save ZIP to disk
			FString ZipPath = FPaths::ProjectSavedDir() / TEXT("GeneratedMaterial.zip");
			FString ExtractDir = FPaths::ProjectSavedDir() / TEXT("GeneratedMaterial");

			if (!FFileHelper::SaveArrayToFile(ZipData, *ZipPath))
			{
				UE_LOG(LogTemp, Error, TEXT("❌ Failed to save ZIP to disk."));
				return;
			}

			// Step 4: Clear & recreate extract folder
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			if (PlatformFile.DirectoryExists(*ExtractDir))
			{
				PlatformFile.DeleteDirectoryRecursively(*ExtractDir);
			}
			PlatformFile.CreateDirectory(*ExtractDir);

			// Step 5: Cross-platform unzip
			FString ExtractCmd;
#if PLATFORM_WINDOWS
			ExtractCmd = FString::Printf(TEXT("powershell -Command \"Expand-Archive -Force '%s' '%s'\""), *ZipPath, *ExtractDir);
#elif PLATFORM_LINUX || PLATFORM_MAC
			ExtractCmd = FString::Printf(TEXT("unzip -o '%s' -d '%s'"), *ZipPath, *ExtractDir);
#else
			UE_LOG(LogTemp, Error, TEXT("❌ Unsupported platform for extraction."));
			return;
#endif

			int32 ReturnCode;
			FString StdOut, StdErr;
			FPlatformProcess::ExecProcess(
#if PLATFORM_WINDOWS
				TEXT("cmd.exe"), *FString::Printf(TEXT("/c %s"), *ExtractCmd),
#else
				TEXT("/bin/sh"), *FString::Printf(TEXT("-c \"%s\""), *ExtractCmd),
#endif
				&ReturnCode, &StdOut, &StdErr
			);

			if (ReturnCode != 0)
			{
				UE_LOG(LogTemp, Error, TEXT("❌ Extraction failed. Error: %s"), *StdErr);
				return;
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("✅ Extraction succeeded."));
			}

			// Step 6: Load Textures
			auto LoadTextureFromPNG = [](const FString& FilePath) -> UTexture2D*
			{
				TArray<uint8> ImageData;
				if (!FFileHelper::LoadFileToArray(ImageData, *FilePath))
				{
					UE_LOG(LogTemp, Error, TEXT("❌ Failed to load image file: %s"), *FilePath);
					return nullptr;
				}

				IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
				TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

				if (ImageWrapper->SetCompressed(ImageData.GetData(), ImageData.Num()))
				{
					TArray<uint8> UncompressedBGRA;
					if (ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, UncompressedBGRA))
					{
						UTexture2D* NewTexture = UTexture2D::CreateTransient(
							ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), PF_B8G8R8A8);

						if (NewTexture)
						{
							void* TextureData = NewTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
							FMemory::Memcpy(TextureData, UncompressedBGRA.GetData(), UncompressedBGRA.Num());
							NewTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
							NewTexture->UpdateResource();
							return NewTexture;
						}
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("❌ Failed to decompress image: %s"), *FilePath);
					}
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("❌ Failed to decode PNG image format: %s"), *FilePath);
				}

				return nullptr;
			};

			FString BaseTexturePath = ExtractDir / TEXT("base_texture.png");
			FString NormalTexturePath = ExtractDir / TEXT("normal_map.png");
			FString RoughnessTexturePath = ExtractDir / TEXT("roughness_map.png");

			UTexture2D* BaseColorTexture = LoadTextureFromPNG(BaseTexturePath);
			UTexture2D* NormalTexture = LoadTextureFromPNG(NormalTexturePath);
			UTexture2D* RoughnessTexture = LoadTextureFromPNG(RoughnessTexturePath);

			if (!BaseColorTexture)
			{
				UE_LOG(LogTemp, Error, TEXT("❌ Failed to load BaseColor texture."));
				return;
			}

			LastBaseColorTexture = BaseColorTexture;
			LastNormalTexture = NormalTexture;
			LastRoughnessTexture = RoughnessTexture;

			PreviewTexture = BaseColorTexture;
			FSlateBrush* NewBrush = new FSlateBrush();
			NewBrush->SetResourceObject(PreviewTexture);
			NewBrush->ImageSize = FVector2D(256, 256);
			PreviewBrush = MakeShareable(NewBrush);

			UE_LOG(LogTemp, Log, TEXT("✅ Textures loaded and preview updated."));
		}
	);
	HttpRequest->ProcessRequest();
}


void FMateriAIModule::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString OriginalPrompt)
{
	if (bWasSuccessful && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode()))
	{
		RetryCount = 0;
		bIsGenerating = false;

		if (bOnlyGenerateBaseTexture)
		{
			HandleBaseTextureResponse(Response->GetContent());
		}
		else
		{
			HandleMaterialZipResponse(Response->GetContent());
		}
	}
	else
	{
		RetryCount++;
		if (RetryCount < MaxRetryAttempts)
		{
			UE_LOG(LogTemp, Warning, TEXT("⚠️ Request failed, retrying (%d/%d)..."), RetryCount, MaxRetryAttempts);
			SendPromptToServer(OriginalPrompt); // Retry
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("❌ Failed after %d attempts."), MaxRetryAttempts);
			bIsGenerating = false;
			RetryCount = 0;
		}
	}
}

UTexture2D* FMateriAIModule::LoadTextureFromFile(const FString& Path)
{
	if (!FPaths::FileExists(Path)) return nullptr;

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *Path)) return nullptr;

	IImageWrapperModule& ImgWrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	TSharedPtr<IImageWrapper> Wrapper = ImgWrapper.CreateImageWrapper(EImageFormat::PNG);

	if (!Wrapper->SetCompressed(FileData.GetData(), FileData.Num())) return nullptr;

	TArray<uint8> Uncompressed;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Uncompressed)) return nullptr;

	UTexture2D* Texture = UTexture2D::CreateTransient(Wrapper->GetWidth(), Wrapper->GetHeight(), PF_B8G8R8A8);
	void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, Uncompressed.GetData(), Uncompressed.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();

	return Texture;
}

UMaterialInstanceDynamic* FMateriAIModule::CreateMaterial(UTexture2D* BaseColor, UTexture2D* Normal, UTexture2D* Roughness)
{
	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/MateriAI/M_BaseMaterial.M_BaseMaterial"));

	if (!BaseMaterial) return nullptr;

	UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(BaseMaterial, nullptr);
	if (BaseColor) DynMat->SetTextureParameterValue("BaseColor", BaseColor);
	if (Normal)    DynMat->SetTextureParameterValue("Normal", Normal);
	if (Roughness) DynMat->SetTextureParameterValue("Roughness", Roughness);

	return DynMat;
}

void FMateriAIModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(MateriAITabName);
}

void FMateriAIModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
		Section.AddMenuEntryWithCommandList(FMateriAICommands::Get().OpenPluginWindow, PluginCommands);
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar");
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("Settings");
		FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FMateriAICommands::Get().OpenPluginWindow));
		Entry.SetCommandList(PluginCommands);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMateriAIModule, MateriAI)
