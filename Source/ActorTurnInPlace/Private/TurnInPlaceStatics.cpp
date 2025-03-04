﻿// Copyright (c) 2025 Jared Taylor


#include "TurnInPlaceStatics.h"

#include "TurnInPlace.h"
#include "TurnInPlaceTypes.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimInstance.h"
#include "EngineDefines.h"  // For UE_ENABLE_DEBUG_DRAWING

#include UE_INLINE_GENERATED_CPP_BY_NAME(TurnInPlaceStatics)

void UTurnInPlaceStatics::SetCharacterMovementType(ACharacter* Character, ECharacterMovementType MovementType)
{
	if (IsValid(Character) && Character->GetCharacterMovement())
	{
		switch (MovementType)
		{
		case ECharacterMovementType::OrientToMovement:
			Character->bUseControllerRotationYaw = false;
		    Character->GetCharacterMovement()->bOrientRotationToMovement = true;
			Character->GetCharacterMovement()->bUseControllerDesiredRotation = false;
			break;
		case ECharacterMovementType::StrafeDesired:
			Character->bUseControllerRotationYaw = false;
			Character->GetCharacterMovement()->bOrientRotationToMovement = false;
			Character->GetCharacterMovement()->bUseControllerDesiredRotation = true;
			break;
		case ECharacterMovementType::StrafeDirect:
			Character->bUseControllerRotationYaw = true;
			Character->GetCharacterMovement()->bOrientRotationToMovement = false;
			Character->GetCharacterMovement()->bUseControllerDesiredRotation = false;
			break;
		}
	}
}

float UTurnInPlaceStatics::GetTurnInPlacePlayRate_ThreadSafe(const FTurnInPlaceAnimGraphData& AnimGraphData,
	bool bForceTurnRateMaxAngle, bool& bHasReachedMaxAngle)
{
	// Check if we've reached the max angle, or if we're forcing the max angle
	bHasReachedMaxAngle = bForceTurnRateMaxAngle;
	if (!bForceTurnRateMaxAngle)
	{
		if (AnimGraphData.bHasValidTurnAngles)
		{
			// Check if we're near the max angle
			bHasReachedMaxAngle |= FMath::IsNearlyEqual(FMath::Abs(AnimGraphData.TurnOffset), AnimGraphData.TurnAngles.MaxTurnAngle);
		}
	}

	// Rate changes, usually increases, when we're at the max angle to keep up with a player turning the camera (control rotation) quickly
	const float MaxAngleRate = bHasReachedMaxAngle ? AnimGraphData.AnimSet.PlayRateAtMaxAngle : 1.f;

	// Detect a change in direction and apply a rate change, so that if we're currently turning left and the player
	// wants to turn right, we speed up the turn rate so they can complete their old turn faster
	const bool bWantsTurnRight = AnimGraphData.TurnOffset > 0.f;
	const bool bDirectionChange = AnimGraphData.bIsTurning && bWantsTurnRight != AnimGraphData.bTurnRight;
	const float DirectionChangeRate = bDirectionChange ? AnimGraphData.AnimSet.PlayRateOnDirectionChange : 1.f;

	// Rates below 1.0 are not supported with this logic
	return FMath::Max(MaxAngleRate, DirectionChangeRate);
}

float UTurnInPlaceStatics::GetUpdatedTurnInPlaceAnimTime_ThreadSafe(const UAnimSequence* TurnAnimation, float CurrentAnimTime,
	float DeltaTime, float TurnPlayRate)
{
	if (!TurnAnimation)
	{
		return CurrentAnimTime;
	}

	const float Accumulate = DeltaTime * TurnPlayRate * TurnAnimation->RateScale;
	return FMath::Min(CurrentAnimTime + Accumulate, TurnAnimation->GetPlayLength());
}

float UTurnInPlaceStatics::GetAnimationSequencePlayRate(const UAnimSequenceBase* Animation)
{
	return Animation ? Animation->RateScale : 1.f;
}

FString UTurnInPlaceStatics::GetAnimationSequenceName(const UAnimSequenceBase* Animation)
{
	return Animation ? Animation->GetName() : "None";
}

void UTurnInPlaceStatics::DebugTurnInPlace(UObject* WorldContextObject, bool bDebug)
{
#if UE_ENABLE_DEBUG_DRAWING
	// Exec all debug commands
	const FString DebugState = bDebug ? TEXT(" 1") : TEXT(" 0");
	UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, TEXT("p.Turn.Debug.TurnOffset") + DebugState);
	UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, TEXT("p.Turn.Debug.TurnOffset.Arrow") + DebugState);
	UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, TEXT("p.Turn.Debug.ActorDirection.Arrow") + DebugState);
	UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, TEXT("p.Turn.Debug.ControlDirection.Arrow") + DebugState);
#endif
}

UAnimSequence* UTurnInPlaceStatics::GetTurnInPlaceAnimation(const FTurnInPlaceAnimSet& AnimSet,
	const FTurnInPlaceGraphNodeData& NodeData, bool bRecovery)
{
	const bool bTurnRight = bRecovery ? NodeData.bIsRecoveryTurningRight : NodeData.bIsTurningRight;
	const TArray<UAnimSequence*>& TurnAnimations = bTurnRight ? AnimSet.RightTurns : AnimSet.LeftTurns;
	return TurnAnimations.IsValidIndex(NodeData.StepSize) ? TurnAnimations[NodeData.StepSize] : nullptr;
}

void UTurnInPlaceStatics::UpdateTurnInPlace(UTurnInPlace* TurnInPlace, float DeltaTime,
	FTurnInPlaceAnimGraphData& AnimGraphData, bool bIsStrafing, FTurnInPlaceAnimGraphOutput& Output,
	bool& bCanUpdateTurnInPlace)
{
	AnimGraphData = FTurnInPlaceAnimGraphData();
	bCanUpdateTurnInPlace = false;
	
	if (!TurnInPlace || !TurnInPlace->HasValidData())
	{
		return;
	}
	
	AnimGraphData = TurnInPlace->UpdateAnimGraphData(DeltaTime);
	bCanUpdateTurnInPlace = true;

	// The pseudo anim state needs to update here
	if (AnimGraphData.bWantsPseudoAnimState)
	{
		ThreadSafeUpdateTurnInPlace_Internal(AnimGraphData, bCanUpdateTurnInPlace, bIsStrafing, Output);
	}

	TurnInPlace->PostUpdateAnimGraphData(DeltaTime, AnimGraphData, Output);
}

void UTurnInPlaceStatics::ThreadSafeUpdateTurnInPlace(const FTurnInPlaceAnimGraphData& AnimGraphData,
	bool bCanUpdateTurnInPlace, bool bIsStrafing, FTurnInPlaceAnimGraphOutput& Output)
{
	if (!AnimGraphData.bWantsPseudoAnimState)
	{
		ThreadSafeUpdateTurnInPlace_Internal(AnimGraphData, bCanUpdateTurnInPlace, bIsStrafing, Output);
	}
}

void UTurnInPlaceStatics::ThreadSafeUpdateTurnInPlace_Internal(const FTurnInPlaceAnimGraphData& AnimGraphData,
	bool bCanUpdateTurnInPlace, bool bIsStrafing, FTurnInPlaceAnimGraphOutput& Output)
{
	if (!bCanUpdateTurnInPlace)
	{
		return;
	}

	// Turn anim graph properties
	Output.TurnOffset = AnimGraphData.TurnOffset;

	// Turn anim graph transitions
	Output.bWantsToTurn = AnimGraphData.bWantsToTurn;
	Output.bWantsTurnRecovery = !AnimGraphData.bIsTurning;

	// Locomotion anim graph transitions
	Output.bTransitionStartToCycleFromTurn = bIsStrafing && FMath::Abs(AnimGraphData.TurnOffset) > AnimGraphData.TurnAngles.MinTurnAngle;
	Output.bTransitionStopToIdleForTurn = AnimGraphData.bIsTurning || AnimGraphData.bWantsToTurn;

	// Play turn anim
	Output.bPlayTurnAnim = Output.bWantsToTurn && !AnimGraphData.bWantsPseudoAnimState;
}

FTurnInPlaceCurveValues UTurnInPlaceStatics::ThreadSafeUpdateTurnInPlaceCurveValues(const UAnimInstance* AnimInstance, const FTurnInPlaceAnimGraphData& AnimGraphData)
{
	FTurnInPlaceCurveValues CurveValues;

	// Turn anim graph curve values
	CurveValues.RemainingTurnYaw = AnimInstance->GetCurveValue(AnimGraphData.Settings.TurnYawCurveName);
	CurveValues.TurnYawWeight = AnimInstance->GetCurveValue(AnimGraphData.Settings.TurnWeightCurveName);

	return CurveValues;
}

void UTurnInPlaceStatics::ThreadSafeUpdateTurnInPlaceNode(FTurnInPlaceGraphNodeData& NodeData,
	const FTurnInPlaceAnimGraphData& AnimGraphData, const FTurnInPlaceAnimSet& AnimSet)
{
	// Retain play rate at max angle for this current turn, if we ever reached it
	// This prevents micro jitters with mouse turning when it constantly re-enters max angle
	bool bHasReachedMaxAngle;
	NodeData.TurnPlayRate = GetTurnInPlacePlayRate_ThreadSafe(AnimGraphData, NodeData.bHasReachedMaxTurnAngle, bHasReachedMaxAngle);
	NodeData.bHasReachedMaxTurnAngle = AnimSet.bMaintainMaxAnglePlayRate && bHasReachedMaxAngle;
}
