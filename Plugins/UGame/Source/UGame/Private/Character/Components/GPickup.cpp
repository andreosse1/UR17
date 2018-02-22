// Copyright 2018, Institute for Artificial Intelligence - University of Bremen
// Author: Waldemar Zeitler

#define PLUGIN_TAG "UGame"
#define TAG_KEY_PICKUP "Pickup"
#define STACKCHECK_RANGE 500.0f
#define PICKUP_SHADOW_SCALE_FACTOR 0.4f // The scale factor when testing collisions on pickup

#include "GPickup.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "../Private/Character/GameController.h"
#include "TagStatics.h"
#include "Engine.h"

// Sets default values for this component's properties
UGPickup::UGPickup()
	:RotationValue(0)
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

	MaximumMassToCarry = 10.0f;
	bMassEffectsMovementSpeed = true;
	bUseQuadratricEquationForSpeedCalculation = true;

	bTwoHandMode = true;
	bStackModeEnabled = true;
	bNeedBothHands = true;
	bBothHandsDependOnMass = true;
	MassThresholdBothHands = 1.5f;

	bBothHandsDependOnVolume = false;
	VolumeThresholdBothHands = 3000.0f;

	bCheckForCollisionsOnPickup = false;
	bCheckForCollisionsOnDrop = true;
	// *** *** *** *** *** *** *** ***

	TransparentMaterial = nullptr;
}


// Called when the game starts
void UGPickup::BeginPlay()
{
	Super::BeginPlay();

	SetOfPickupItems = FTagStatics::GetActorSetWithKeyValuePair(GetWorld(), PLUGIN_TAG, TAG_KEY_PICKUP, "True");

	if (PlayerCharacter == nullptr) UE_LOG(LogTemp, Fatal, TEXT("UCPickup::BeginPlay: The PlayerCharacter was not assigned. Restarting the editor might fix this."));

	RaycastRange = PlayerCharacter->GraspRange;

	UInputComponent* PlayerInputComponent = PlayerCharacter->InputComponent;

	PlayerInputComponent->BindAction("CancelAction", IE_Pressed, this, &UGPickup::CancelActions);
	PlayerInputComponent->BindAction("CancelAction", IE_Released, this, &UGPickup::StopCancelActions);

	// Create Static mesh actors for hands to weld items we pickup into this position
	if (PlayerCharacter->LeftHandPosition == nullptr || PlayerCharacter->RightHandPosition == nullptr || PlayerCharacter->BothHandPosition == nullptr) {
		GEngine->AddOnScreenDebugMessage(-1, 100000.0f, FColor::Red, "No hand actors found. Game paused", false);
		GetWorld()->GetFirstPlayerController()->SetPause(true);
	}

	// Spawn new actors and set them up
	LeftHandActor = GetWorld()->SpawnActor<AStaticMeshActor>();
	RightHandActor = GetWorld()->SpawnActor<AStaticMeshActor>();
	BothHandActor = GetWorld()->SpawnActor<AStaticMeshActor>();

	LeftHandActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	RightHandActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	BothHandActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);

	LeftHandActor->SetActorLocation(PlayerCharacter->LeftHandPosition->GetActorLocation());
	RightHandActor->SetActorLocation(PlayerCharacter->RightHandPosition->GetActorLocation());
	BothHandActor->SetActorLocation(PlayerCharacter->BothHandPosition->GetActorLocation());

	LeftHandActor->AttachToActor(PlayerCharacter, FAttachmentTransformRules::KeepWorldTransform);
	RightHandActor->AttachToActor(PlayerCharacter, FAttachmentTransformRules::KeepWorldTransform);
	BothHandActor->AttachToActor(PlayerCharacter, FAttachmentTransformRules::KeepWorldTransform);

	LeftHandActor->GetStaticMeshComponent()->SetSimulatePhysics(false);
	RightHandActor->GetStaticMeshComponent()->SetSimulatePhysics(false);
	BothHandActor->GetStaticMeshComponent()->SetSimulatePhysics(false);

	LeftHandActor->GetStaticMeshComponent()->SetCollisionProfileName("NoCollision");
	RightHandActor->GetStaticMeshComponent()->SetCollisionProfileName("NoCollision");
	BothHandActor->GetStaticMeshComponent()->SetCollisionProfileName("NoCollision");
	// *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***


	MaxMovementSpeed = PlayerCharacter->MovementComponent->MaxMovementSpeed;
}


// Called every frame
void UGPickup::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	bool bLockedByOtherComponent = PlayerCharacter->LockedByComponent != nullptr &&  PlayerCharacter->LockedByComponent != this;

	if (bLockedByOtherComponent == false && bAllCanceled == false) {
		ItemToHandle = PlayerCharacter->FocussedActor;

		if (bRightMouseHold) {
			OnInteractionKeyHold(true);
		}
		else if (bLeftMouseHold) {
			OnInteractionKeyHold(false);
		}
	}
}

void UGPickup::StartPickup()
{
	if (bTwoHandMode == false && (ItemInRightHand != nullptr || ItemInLeftHand != nullptr)) return; // We only have one hand but already holding something
	SetLockedByComponent(true);

	PlayerCharacter->bRaytraceEnabled = false;
	if (bStackModeEnabled == false) {
		BaseItemToPick = ItemToHandle;
	}
	else {
		BaseItemToPick = GetItemStack(ItemToHandle);
	}

	UStaticMeshComponent* MeshComponent = BaseItemToPick->GetStaticMeshComponent();
	MeshComponent->SetSimulatePhysics(true);

	// Check mass
	MassOfLastItemPickedUp = MeshComponent->GetMass();

	if (MassOfLastItemPickedUp + MassToCarry > MaximumMassToCarry) {
		GEngine->AddOnScreenDebugMessage(1, 3.0f, FColor::Red, "Object too heavy.", false);
		UE_LOG(LogTemp, Warning, TEXT("Object too heavy %f kg"), MassOfLastItemPickedUp + MassToCarry);
		MassOfLastItemPickedUp = 0;

		UnstackItems(BaseItemToPick);

		BaseItemToPick = nullptr;
		PlayerCharacter->bRaytraceEnabled = true;
		return;
	}
	//  *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***

	if (bMassEffectsMovementSpeed) SetMovementSpeed(MassOfLastItemPickedUp);

	// Check if both hands are needed
	if (bNeedBothHands && CalculateIfBothHandsNeeded()) {
		// Both hands needed
		if (bTwoHandMode && ItemInRightHand == nullptr && ItemInLeftHand == nullptr) {
			UsedHand = EHand::Both;
		}
		else {
			GEngine->AddOnScreenDebugMessage(1, 3.0f, FColor::Red, "Object too heavy. Both hands needed!", false);

			UnstackItems(BaseItemToPick);

			BaseItemToPick = nullptr;
			PlayerCharacter->bRaytraceEnabled = true;
			return;
		}
	}
	// *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
}

void UGPickup::PickupItem()
{
	if (BaseItemToPick == nullptr || bItemCanBePickedUp == false) {
		SetMovementSpeed(-MassOfLastItemPickedUp);
		return;
	}

	FAttachmentTransformRules TransformRules = FAttachmentTransformRules::KeepWorldTransform;
	TransformRules.bWeldSimulatedBodies = true;

	if (UsedHand == EHand::Right) {
		if (ItemInRightHand != nullptr) return; // We already carry something 
		BaseItemToPick->AttachToActor(RightHandActor, TransformRules);
		ItemInRightHand = BaseItemToPick;
	}
	else if (UsedHand == EHand::Left) {
		if (ItemInLeftHand != nullptr) return; // We already carry something 
		BaseItemToPick->AttachToActor(LeftHandActor, TransformRules);
		ItemInLeftHand = BaseItemToPick;
	}
	else if (UsedHand == EHand::Both) {
		if (ItemInRightHand != nullptr || ItemInLeftHand != nullptr) return; // We already carry something 
		BaseItemToPick->AttachToActor(BothHandActor, TransformRules);
		ItemInRightHand = ItemInLeftHand = BaseItemToPick;
	}

	BaseItemToPick->SetActorRelativeLocation(FVector::ZeroVector, false, nullptr, ETeleportType::TeleportPhysics);
	BaseItemToPick->SetActorRelativeRotation(FRotator::ZeroRotator);

	BaseItemToPick->GetStaticMeshComponent()->SetSimulatePhysics(false);

	// Disable collisions
	if (bEnableCollisionOfItemsInHand == false) {
		BaseItemToPick->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		TArray<AActor*> Children;
		BaseItemToPick->GetAttachedActors(Children);
		for (auto& Child : Children) {
			AStaticMeshActor* CastActor = Cast<AStaticMeshActor>(Child);
			if (CastActor != nullptr) CastActor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	BaseItemToPick = nullptr;

	PlayerCharacter->bRaytraceEnabled = true;
}

void UGPickup::ShadowPickupItem()
{
	if (ItemToHandle == nullptr) return;
	if (BaseItemToPick == nullptr) return;

	DisableShadowItems();

	FVector HandPosition;

	if (UsedHand == EHand::Right) {
		HandPosition = RightHandActor->GetActorLocation();
	}
	else if (UsedHand == EHand::Left) {
		HandPosition = LeftHandActor->GetActorLocation();
	}
	else {
		HandPosition = BothHandActor->GetActorLocation();
	}

	TArray<AActor*> ChildItemsOfBaseItem;
	BaseItemToPick->GetAttachedActors(ChildItemsOfBaseItem);

	AStaticMeshActor* ShadowRoot = GetNewShadowItem(BaseItemToPick);
	ShadowItems.Add(ShadowRoot);

	// Create new shadow items for each child
	for (auto& ItemToShadow : ChildItemsOfBaseItem) {
		AStaticMeshActor* CastActor = Cast<AStaticMeshActor>(ItemToShadow);

		if (CastActor == nullptr) return;

		AStaticMeshActor* ShadowItem = GetNewShadowItem(CastActor);
		ShadowItems.Add(ShadowItem);

		ShadowItem->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		FAttachmentTransformRules TransformRules = FAttachmentTransformRules::KeepWorldTransform;
		TransformRules.bWeldSimulatedBodies = true;

		ShadowItem->AttachToActor(ShadowRoot, TransformRules);
	}
	//  *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***

	ShadowRoot->SetActorLocation(HandPosition);

	if (bCheckForCollisionsOnPickup) {
		// Check for collisions in between
		FVector CamLoc;
		FRotator CamRot;
		PlayerCharacter->Controller->GetPlayerViewPoint(CamLoc, CamRot); // Get the camera position and rotation

		FHitResult RaycastHit = RaytraceWithIgnoredActors(ChildItemsOfBaseItem);

		if (RaycastHit.Location != FVector::ZeroVector) {
			ShadowRoot->SetActorScale3D(FVector(PICKUP_SHADOW_SCALE_FACTOR));
			FVector FromPosition = RaycastHit.Location;
			FVector ToPosition = CamLoc;

			// *** Ignored Actors
			TArray<AActor*> IgnoredActors = ChildItemsOfBaseItem; // ignore items to pickup 
			IgnoredActors.Add(BaseItemToPick);
			IgnoredActors.Append(ShadowItems); //All shadow items are ignored
			IgnoredActors.Add(ItemInLeftHand);
			IgnoredActors.Add(ItemInRightHand);

			// Add all children in hands to ignored actors
			if (ItemInLeftHand != nullptr) {
				TArray<AActor*> ChildrenInLeftHand;
				ItemInLeftHand->GetAttachedActors(ChildrenInLeftHand);

				IgnoredActors.Append(ChildrenInLeftHand);
			}
			if (ItemInRightHand != nullptr) {
				TArray<AActor*> ChildrenInRightHand;
				ItemInRightHand->GetAttachedActors(ChildrenInRightHand);

				IgnoredActors.Append(ChildrenInRightHand);
			}
			// *****************

			FHitResult SweepHit = CheckForCollision(FromPosition, ToPosition, ShadowRoot, IgnoredActors);

			ShadowRoot->SetActorScale3D(FVector(1.0f));

			if (SweepHit.GetActor() != nullptr) {
				ShadowRoot->SetActorLocation(SweepHit.Location);
				bItemCanBePickedUp = false;
			}
			else {
				ShadowRoot->SetActorLocation(HandPosition);
				bItemCanBePickedUp = true;
			}
		}
	}
	else {
		ShadowRoot->SetActorLocation(HandPosition);
		bItemCanBePickedUp = true;
	}

	ShadowBaseItem = ShadowRoot;
}

TArray<AStaticMeshActor*> UGPickup::FindAllStackableItems(AStaticMeshActor* ActorToPickup)
{
	TArray<AStaticMeshActor*>  StackedItems;
	int countNewItemsFound = 0;
	do
	{
		countNewItemsFound = 0;
		TArray<AActor*> IgnoredActors;

		for (auto &elem : StackedItems) {
			IgnoredActors.Add(elem);
		}

		FVector SecondPointOfSweep = ActorToPickup->GetActorLocation() + FVector::UpVector * STACKCHECK_RANGE;
		TArray<FHitResult> SweptActors;
		FComponentQueryParams Params;
		Params.AddIgnoredComponent_LikelyDuplicatedRoot(ActorToPickup->GetStaticMeshComponent());
		Params.AddIgnoredActors(IgnoredActors);
		Params.bTraceComplex = true;
		Params.bFindInitialOverlaps = true;

		ActorToPickup->GetWorld()->ComponentSweepMulti(
			SweptActors,
			ActorToPickup->GetStaticMeshComponent(),
			ActorToPickup->GetActorLocation(),
			SecondPointOfSweep,
			ActorToPickup->GetActorRotation(),
			Params);

		for (auto& elem : SweptActors) {
			if (elem.GetActor() != nullptr) {
				if (SetOfPickupItems.Contains(elem.GetActor())) {
					AStaticMeshActor* CastedActor = Cast<AStaticMeshActor>(elem.GetActor());

					if (StackedItems.Contains(CastedActor)) continue;

					UE_LOG(LogTemp, Warning, TEXT("Found item while sweeping %s"), *elem.GetActor()->GetName());
					FVector elemLocation = elem.GetActor()->GetActorLocation();
					FVector ActorToPickupLocation = ActorToPickup->GetActorLocation();

					if (elemLocation.Z < ActorToPickupLocation.Z) {
						UE_LOG(LogTemp, Warning, TEXT("Ignoring lower item %s"), *elem.GetActor()->GetName());
						continue; // ignore if the found item is lower than base item 
					}

					FVector DeltaPosition = elemLocation - ActorToPickupLocation;

					if (CastedActor != nullptr) {
						StackedItems.Add(CastedActor);
						countNewItemsFound++;
					}
				}
				else if (elem.GetActor()->GetActorLocation().Z >= ActorToPickup->GetActorLocation().Z) {
					break; // Break at non-pickupable item which is above our base item (It's probably a shelf or something)

				}
			}
		}
	} while (countNewItemsFound > 0);

	return StackedItems;
}

AStaticMeshActor * UGPickup::GetItemStack(AStaticMeshActor * BaseItem)
{
	TArray<AStaticMeshActor*>  StackOfItems = FindAllStackableItems(BaseItem);

	StackOfItems.Remove(BaseItem);

	for (auto& ChildItem : StackOfItems) {
		FAttachmentTransformRules TransformRules = FAttachmentTransformRules::KeepWorldTransform;
		TransformRules.bWeldSimulatedBodies = true;

		ChildItem->AttachToActor(BaseItem, TransformRules);
	}

	return BaseItem;
}

AStaticMeshActor * UGPickup::GetNewShadowItem(AStaticMeshActor * FromActor)
{
	FActorSpawnParameters Parameters;
	AStaticMeshActor* NewShadowPickupItem = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Parameters);

	NewShadowPickupItem->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	NewShadowPickupItem->GetStaticMeshComponent()->SetSimulatePhysics(false);
	NewShadowPickupItem->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

	NewShadowPickupItem->GetStaticMeshComponent()->SetStaticMesh(FromActor->GetStaticMeshComponent()->GetStaticMesh());
	NewShadowPickupItem->SetActorLocationAndRotation(FromActor->GetActorLocation(), FromActor->GetActorRotation());

	for (size_t i = 0; i < NewShadowPickupItem->GetStaticMeshComponent()->GetMaterials().Num(); i++)
	{
		NewShadowPickupItem->GetStaticMeshComponent()->SetMaterial(i, TransparentMaterial);
	}

	return NewShadowPickupItem;
}

void UGPickup::UnstackItems(AStaticMeshActor * BaseItem)
{
	if (BaseItem == nullptr) return;

	TArray<AActor*> ChildItemsOfBaseItem;
	BaseItem->GetAttachedActors(ChildItemsOfBaseItem);

	BaseItem->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	BaseItem->GetStaticMeshComponent()->SetSimulatePhysics(true);
	BaseItem->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	for (auto& DroppedItem : ChildItemsOfBaseItem) {
		AStaticMeshActor* CastActor = Cast<AStaticMeshActor>(DroppedItem);
		DroppedItem->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

		if (CastActor != nullptr) {
			CastActor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			CastActor->GetStaticMeshComponent()->SetSimulatePhysics(true);
		}
	}
}

FHitResult UGPickup::CheckForCollision(FVector From, FVector To, AStaticMeshActor * ItemToSweep, TArray<AActor*> IgnoredActors)
{
	TArray<FHitResult> Hits;
	FComponentQueryParams Params;

	Params.AddIgnoredActors(IgnoredActors);
	Params.AddIgnoredActor(GetOwner()); // Always ignore player

	GetWorld()->ComponentSweepMulti(Hits, ItemToSweep->GetStaticMeshComponent(), From, To, ItemToSweep->GetActorRotation(), Params);

	if (Hits.Num() > 0) {
		return Hits[0];
	}

	return FHitResult();
}

void UGPickup::DisableShadowItems()
{
	for (auto& ShdwItm : ShadowItems) {
		ShdwItm->Destroy();
	}

	ShadowItems.Empty();

	if (ShadowBaseItem != nullptr)
	{
		ShadowBaseItem->Destroy();
		ShadowBaseItem = nullptr;
	}
}

FVector UGPickup::GetPositionOnSurface(AActor * Item, FVector PointOnSurface)
{
	FVector ItemOrigin;
	FVector ItemBoundExtend;
	Item->GetActorBounds(false, ItemOrigin, ItemBoundExtend);

	FVector DeltaOfPivotToCenter = ItemOrigin - Item->GetActorLocation();

	return FVector(PointOnSurface.X, PointOnSurface.Y, PointOnSurface.Z + ItemBoundExtend.Z) - DeltaOfPivotToCenter;
}

FHitResult UGPickup::RaytraceWithIgnoredActors(TArray<AActor*> IgnoredActors, FVector StartOffset, FVector TargetOffset)
{
	FHitResult RaycastResult;

	FVector CamLoc;
	FRotator CamRot;

	PlayerCharacter->Controller->GetPlayerViewPoint(CamLoc, CamRot); // Get the camera position and rotation
	const FVector StartTrace = CamLoc + StartOffset; // trace start is the camera location
	const FVector Direction = CamRot.Vector();
	const FVector EndTrace = StartTrace + Direction * RaycastRange + TargetOffset; // and trace end is the camera location + an offset in the direction

	FCollisionQueryParams TraceParams;
	TraceParams.AddIgnoredActors(IgnoredActors);


	TArray<AActor*> ShadowActors;
	for (auto& shadow : ShadowItems) {
		ShadowActors.Add(shadow);
	}
	TraceParams.AddIgnoredActors(ShadowActors); // Always ignore shadow item

	if (ShadowBaseItem != nullptr) TraceParams.AddIgnoredActor(ShadowBaseItem); // Always ignore shadow item
	TraceParams.AddIgnoredActor(GetOwner()); // Always ignore player

	GetWorld()->LineTraceSingleByChannel(RaycastResult, StartTrace, EndTrace, ECollisionChannel::ECC_Visibility, TraceParams);

	return RaycastResult;
}

void UGPickup::SetupKeyBindings(UInputComponent* PlayerInputComponent)
{
	PlayerInputComponent->BindAction("LeftHandAction", IE_Pressed, this, &UGPickup::InputLeftHandPressed);
	PlayerInputComponent->BindAction("LeftHandAction", IE_Released, this, &UGPickup::InputLeftHandReleased);

	PlayerInputComponent->BindAction("RightHandAction", IE_Pressed, this, &UGPickup::InputRightHandPressed);
	PlayerInputComponent->BindAction("RightHandAction", IE_Released, this, &UGPickup::InputRightHandReleased);

	// Rotation mode after pressing Tab (by Waldemar Zeitler)
	PlayerInputComponent->BindAction("RotationMode", IE_Pressed, this, &UGPickup::RotationMode);
}

void UGPickup::InputLeftHandPressed()
{
	if (bLeftMouseHold) return; // We already clicked that key
	if (bRightMouseHold) return; // Ignore this call if the other key has been pressed
	if (bIsStackChecking) return; // Don't react if stackchecking is active

	bLeftMouseHold = true;
	UsedHand = EHand::Left;
	OnInteractionKeyPressed(false);
}

void UGPickup::InputLeftHandReleased()
{
	if (bLeftMouseHold == false) return; // We can't release if we didn't clicked first
	if (bRightMouseHold) return; // Ignore this call if the other key has been pressed
	if (bIsStackChecking) return; // Don't react if stackchecking is active

	bLeftMouseHold = false;
	OnInteractionKeyReleased(false);
}

void UGPickup::InputRightHandPressed()
{
	if (bRightMouseHold) return; // We already clicked that key
	if (bLeftMouseHold) return; // Ignore this call if the other key has been pressed
	if (bIsStackChecking) return; // Don't react if stackchecking is active

	bRightMouseHold = true;
	UsedHand = EHand::Right;
	OnInteractionKeyPressed(true);
}

void UGPickup::InputRightHandReleased()
{
	if (bRightMouseHold == false) return;  // We can't release if we didn't clicked first
	if (bLeftMouseHold) return; // Ignore this call if the other key has been pressed
	if (bIsStackChecking) return; // Don't react if stackchecking is active

	bRightMouseHold = false;
	OnInteractionKeyReleased(true);
}

void UGPickup::OnInteractionKeyPressed(bool bIsRightKey)
{
	if (ItemToHandle != nullptr) {
		if (SetOfPickupItems.Contains(ItemToHandle)) {
			if (bIsRightKey) {
				// Right hand handling
				if (ItemInRightHand == nullptr) {
					StartPickup();
				}
				else {
					if (bIsItemDropping == false) StartDropItem();
				}
			}
			else {
				// Left hand handling
				if (ItemInLeftHand == nullptr) {
					StartPickup();
				}
				else {
					if (bIsItemDropping == false) StartDropItem();
				}
			}
		}
	}
	else {
		if (bIsItemDropping == false) StartDropItem();
	}
}

void UGPickup::OnInteractionKeyHold(bool bIsRightKey)
{
	if (bIsDragging) {
		DragItem();
	}
	else if (bIsItemDropping) {
		ShadowDropItem();
	}
	else {
		ShadowPickupItem();
	}
}

void UGPickup::OnInteractionKeyReleased(bool bIsRightKey)
{
	if (bIsDragging) {
		EndDrag();
	}
	else if (bIsItemDropping) {
		DropItem();
	}
	else {
		PickupItem();
	}

	ResetComponentState();
}

void UGPickup::ResetComponentState()
{
	DisableShadowItems();
	SetLockedByComponent(false);
	PlayerCharacter->bRaytraceEnabled = true;
	CancelDetachItems();
}

void UGPickup::RotationMode()
{
	if (RotationValue == 0 || RotationValue == 2 && ItemInLeftHand != nullptr) {
		RotationValue = 1;
	}
	else if (RotationValue == 1 || RotationValue == 0 && ItemInRightHand != nullptr) {
		RotationValue = 2;
	} 
	else {
		RotationValue = 0;
	}
}


void UGPickup::OnStackCheckIsDone(bool wasSuccessful)
{

	bStackCheckSuccess = wasSuccessful;

	if (bStackCheckSuccess) {
		UE_LOG(LogTemp, Warning, TEXT("Stackcheck sucessful"));
		BaseItemToPick = GetItemStack(ItemToHandle); // We need to reassign the whole stack
		PickupItem();
	}
	else {
		UE_LOG(LogTemp, Warning, TEXT("Stackcheck failed"));
		if (BaseItemToPick != nullptr) {
			SetMovementSpeed(-MassOfLastItemPickedUp);
			UnstackItems(BaseItemToPick);
		}
	}

	bIsStackChecking = false;

	if (PlayerCharacter->MovementComponent != nullptr) {
		PlayerCharacter->MovementComponent->SetMovable(true);
	}
}

void UGPickup::StartDrag()
{
	ItemToDrag = ItemToHandle;
	bIsDragging = true;

	// Raycast
	TArray<AActor*> IgnoredActors;
	IgnoredActors.Add(ItemToDrag);
	FHitResult RaycastResult = RaytraceWithIgnoredActors(IgnoredActors);

	FVector RayPosition = RaycastResult.Location;
	DeltaVectorToDrag = ItemToDrag->GetActorLocation() - RayPosition;
	DeltaVectorToDrag.Z = 0;

	SetLockedByComponent(true);
}

void UGPickup::DragItem()
{
	if (ItemToDrag == nullptr) return;
	if (UsedHand == EHand::Right && ItemInRightHand != nullptr || UsedHand == EHand::Left && ItemInLeftHand != nullptr || UsedHand == EHand::Both && ItemInLeftHand != nullptr) {
		// We can't drag with a hand which holds an item
		GEngine->AddOnScreenDebugMessage(1, 3.0f, FColor::Red, "Hand(s) not empty. Can't interact", false);
		return;
	}

	TArray<AActor*> IgnoredActors;
	IgnoredActors.Add(ItemToDrag);

	FHitResult RaycastResult = RaytraceWithIgnoredActors(IgnoredActors);

	FVector RayPosition = RaycastResult.Location;

	if (RayPosition.IsZero() == false)
	{
		FVector PointOnSurface = GetPositionOnSurface(ItemToDrag, RayPosition);
		ItemToDrag->SetActorLocation(FVector(PointOnSurface.X, PointOnSurface.Y, ItemToDrag->GetActorLocation().Z) + DeltaVectorToDrag);
	}

}

void UGPickup::EndDrag()
{
	ItemToDrag = nullptr;
	bIsDragging = false;
}

void UGPickup::StartDropItem()
{
	bIsItemDropping = true;
	SetLockedByComponent(true);
}

void UGPickup::DropItem()
{
	// Set the rotation value, because the item is not hold in the hand anymore 
	RotationValue = 0;

	bIsItemDropping = false;
	if (ShadowBaseItem == nullptr) return;

	AStaticMeshActor* ItemToDrop;

	if (UsedHand == EHand::Right) {
		ItemToDrop = ItemInRightHand;
	}
	else {
		ItemToDrop = ItemInLeftHand; // Left hand or both hand item
	}

	if (ItemToDrop == nullptr) return;

	TArray<AActor*> ChildItemsOfBaseItem;
	ItemToDrop->GetAttachedActors(ChildItemsOfBaseItem);

	ItemToDrop->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	ItemToDrop->GetStaticMeshComponent()->SetSimulatePhysics(true);
	ItemToDrop->SetActorLocation(ShadowBaseItem->GetActorLocation());
	ItemToDrop->SetActorRotation(ShadowBaseItem->GetActorRotation());
	ItemToDrop->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	float Mass = 0;

	for (auto& DroppedItem : ChildItemsOfBaseItem) {
		AStaticMeshActor* CastActor = Cast<AStaticMeshActor>(DroppedItem);
		DroppedItem->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

		if (CastActor != nullptr) {
			CastActor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			CastActor->GetStaticMeshComponent()->SetSimulatePhysics(true);

			Mass += CastActor->GetStaticMeshComponent()->GetMass();
		}
	}
	Mass += ItemToDrop->GetStaticMeshComponent()->GetMass();

	if (ItemInRightHand == ItemInLeftHand) {
		ItemInRightHand = ItemInLeftHand = nullptr;
	}
	else if (UsedHand == EHand::Right) {
		ItemInRightHand = nullptr;
	}
	else if (UsedHand == EHand::Left) {
		ItemInLeftHand = nullptr;
	}

	if (bMassEffectsMovementSpeed) {
		SetMovementSpeed(-Mass);
	}
}

void UGPickup::ShadowDropItem()
{
	DisableShadowItems();

	AStaticMeshActor* ItemToDrop;

	if (UsedHand == EHand::Right) {
		ItemToDrop = ItemInRightHand;
	}
	else {
		ItemToDrop = ItemInLeftHand; // Left hand or both hand item
	}

	if (ItemToDrop == nullptr) return;

	TArray<AActor*> ChildItemsOfBaseItem;
	ItemToDrop->GetAttachedActors(ChildItemsOfBaseItem);

	AStaticMeshActor* ShadowRoot = GetNewShadowItem(ItemToDrop);
	ItemToDrop->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ShadowItems.Add(ShadowRoot);

	TArray<AActor*> IgnoredActors; // Actors that get ignored by following raytrace
	IgnoredActors.Add(ItemToDrop);

	for (auto& ItemToShadow : ChildItemsOfBaseItem) {
		AStaticMeshActor* CastActor = Cast<AStaticMeshActor>(ItemToShadow);

		if (CastActor == nullptr) return;
		CastActor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

		AStaticMeshActor* ShadowItem = GetNewShadowItem(CastActor);
		ShadowItems.Add(ShadowItem);

		ShadowItem->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		FAttachmentTransformRules TransformRules = FAttachmentTransformRules::KeepWorldTransform;
		TransformRules.bWeldSimulatedBodies = true;

		ShadowItem->AttachToActor(ShadowRoot, TransformRules);
		IgnoredActors.Add(CastActor);
	}

	FVector CamLoc;
	FRotator CamRot;
	PlayerCharacter->Controller->GetPlayerViewPoint(CamLoc, CamRot); // Get the camera position and rotation

	FHitResult RaycastHit = RaytraceWithIgnoredActors(IgnoredActors);

	if (RaycastHit.Actor != nullptr) {

		if (bCheckForCollisionsOnDrop) {
			ShadowRoot->SetActorRotation(ShadowRoot->GetActorRotation());

			// Checking for collisions between the player and the position the player wants to drop the item
			FVector FromPosition = CamLoc;
			FVector ToPosition = RaycastHit.Location;

			// *** Ignored Actors
			TArray<AActor*> IgnoredActors = ChildItemsOfBaseItem; // ignore items to pickup 
			IgnoredActors.Add(BaseItemToPick);
			IgnoredActors.Append(ShadowItems); //All shadow items are ignored
			IgnoredActors.Add(ItemInLeftHand);
			IgnoredActors.Add(ItemInRightHand);

			// Add all children in hands to ignored actors
			if (ItemInLeftHand != nullptr) {
				TArray<AActor*> ChildrenInLeftHand;
				ItemInLeftHand->GetAttachedActors(ChildrenInLeftHand);

				IgnoredActors.Append(ChildrenInLeftHand);
			}
			if (ItemInRightHand != nullptr) {
				TArray<AActor*> ChildrenInRightHand;
				ItemInRightHand->GetAttachedActors(ChildrenInRightHand);

				IgnoredActors.Append(ChildrenInRightHand);
			}
			// *****************

			FHitResult SweepHit = CheckForCollision(FromPosition, ToPosition, ShadowRoot, IgnoredActors);

			if (SweepHit.GetActor() != nullptr) {
				ShadowRoot->SetActorLocation(SweepHit.Location);
				ShadowBaseItem = ShadowRoot;
			}
		}
		else {
			ShadowRoot->SetActorLocation(GetPositionOnSurface(ItemToDrop, RaycastHit.Location));
			ShadowBaseItem = ShadowRoot;
		}
	}
	else {
		ShadowBaseItem = nullptr; // We didn't hit any surface
	}
}

void UGPickup::CancelActions()
{
	bAllCanceled = true;
	ResetComponentState();


	if (bIsItemDropping == false) {
		// Pick up event canceled
		if (BaseItemToPick != nullptr) {
			SetMovementSpeed(-MassOfLastItemPickedUp);
		}
	}

	BaseItemToPick = nullptr;

	bRightMouseHold = false;
	bLeftMouseHold = false;
}

void UGPickup::StopCancelActions()
{
	bAllCanceled = false;
}

void UGPickup::CancelDetachItems()
{
	if (BaseItemToPick != nullptr) {
		TArray<AActor*> Children;
		BaseItemToPick->GetAttachedActors(Children);
		for (auto& item : Children) {
			AStaticMeshActor* CastActor = Cast<AStaticMeshActor>(item);

			CastActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			CastActor->GetStaticMeshComponent()->SetSimulatePhysics(true);
		}
	}
}

void UGPickup::SetLockedByComponent(bool bIsLocked)
{
	if (bIsLocked) {
		PlayerCharacter->LockedByComponent = this;
	}
	else {
		PlayerCharacter->LockedByComponent = nullptr;
	}
}

bool UGPickup::CalculateIfBothHandsNeeded()
{
	BaseItemToPick->GetStaticMeshComponent()->SetSimulatePhysics(true);
	if (bBothHandsDependOnMass) {
		float MassOfItem = BaseItemToPick->GetStaticMeshComponent()->GetMass();
		UE_LOG(LogTemp, Warning, TEXT("Mass of object %f"), MassOfItem);

		if (MassOfItem >= MassThresholdBothHands) {
			BaseItemToPick->GetStaticMeshComponent()->SetSimulatePhysics(false);
			return true;
		}
	}

	if (bBothHandsDependOnVolume) {
		float Volume = BaseItemToPick->GetStaticMeshComponent()->Bounds.GetBox().GetSize().SizeSquared();
		UE_LOG(LogTemp, Warning, TEXT("Volume of object %f"), Volume);

		if (Volume >= VolumeThresholdBothHands) {
			BaseItemToPick->GetStaticMeshComponent()->SetSimulatePhysics(false);
			return true;
		}
	}
	BaseItemToPick->GetStaticMeshComponent()->SetSimulatePhysics(false);
	return false;
}

void UGPickup::SetMovementSpeed(float Weight)
{
	if (PlayerCharacter->MovementComponent == nullptr) return;
	MassToCarry += Weight;

	float MinSpeed = PlayerCharacter->MovementComponent->MinMovementSpeed;
	float MaxSpeed = PlayerCharacter->MovementComponent->MaxMovementSpeed;

	float NewSpeed = 0;

	if (Weight <= 0 /*We are dropping*/  && ItemInLeftHand == nullptr && ItemInRightHand == nullptr) {	// Check if we still carry something
		PlayerCharacter->MovementComponent->CurrentSpeed = MaxMovementSpeed;
		UE_LOG(LogTemp, Warning, TEXT("New speed %f"), MaxMovementSpeed);
		MassOfLastItemPickedUp = 0;
	}
	else {
		if (bUseQuadratricEquationForSpeedCalculation) {
			NewSpeed = (MinSpeed - MaxSpeed) * MassToCarry * MassToCarry / (MaximumMassToCarry * MaximumMassToCarry) + MaxSpeed;
		}
		else {
			NewSpeed = (MinSpeed - MaxSpeed) * MassToCarry / MaximumMassToCarry + MaxSpeed;
		}

		UE_LOG(LogTemp, Warning, TEXT("New speed %f"), NewSpeed);

		PlayerCharacter->MovementComponent->CurrentSpeed = NewSpeed;
	}
}

