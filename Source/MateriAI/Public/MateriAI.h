#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IHttpRequest.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"


class FToolBarBuilder;
class FMenuBuilder;

class FMateriAIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// Plugin logic
	void PluginButtonClicked();
	void RegisterMenus();
	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);

	// UI & preview
	TSharedPtr<class FUICommandList> PluginCommands;
	TSharedPtr<SMultiLineEditableTextBox> PromptTextBox;
	class UTexture2D* PreviewTexture = nullptr;
	TSharedPtr<struct FSlateBrush> PreviewBrush;

	// Preview state
	enum class EPreviewShape
	{
		Sphere,
		Cube
	};

	TArray<TSharedPtr<FString>> PreviewShapeOptions;
	EPreviewShape CurrentPreviewShape = EPreviewShape::Sphere;

	// State flags
	bool bIsGenerating = false;
	bool bOnlyGenerateBaseTexture = false;
	bool bIsSaving = false;
	FString SelectedImagePath;

	// Actions
	FReply OnPickImageClicked();
	FReply OnGenerateClicked();
	FReply OnSaveClicked();
	void SendPromptToServer(const FString& Prompt);
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	UMaterialInstanceDynamic* CreateMaterial(UTexture2D* BaseColor, UTexture2D* Normal, UTexture2D* Roughness);
	void HandleBaseTextureResponse(const TArray<uint8>& ZipData);
	void HandleMaterialZipResponse(const TArray<uint8>& ZipData);
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString OriginalPrompt);
	UTexture2D* LoadTextureFromFile(const FString& Path);
	FReply OnSave3DClicked();
	UTexture2D* ImportTextureAsset(const FString& FilePath, const FString& DestinationPath);
};
