#include "stdafx.h"
#include "CoDXModelBonesHelper.h"
#include "Strings.h"
#include "DBGameGenerics.h"

void CoDXModelBonesHelper::ReadXModelBones(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    // Our total bone count
    size_t BoneCount = (size_t)Model->BoneCount + Model->CosmeticBoneCount;

    // Iterate and build bones
    for (size_t i = 0; i < BoneCount; i++)
    {
        // Add the new bone
        auto& NewBone = ResultModel->AddBone();

        // Read the ID
        uint64_t BoneID = 0;

        // Read the ID
        // TODO: Switch this out for a bone ID "type" enum for when we port other games to use this helper
        switch (Model->BoneIndexSize)
        {
        case 4: NewBone.TagName = CoDAssets::GetHashedString("bone", (uint64_t)CoDAssets::GameInstance->Read<uint32_t>(Model->BoneIDsPtr + i * 4)); break;
        case 8: NewBone.TagName = CoDAssets::GetHashedString("bone", (uint64_t)CoDAssets::GameInstance->Read<uint32_t>(Model->BoneIDsPtr + i * 8 + 4)); break;
        }

        // Check for empty bone name
        if (Strings::IsNullOrWhiteSpace(NewBone.TagName))
        {
            // Make a new bone name
            if (i == 0)
            {
                NewBone.TagName = "tag_origin";
            }
            else
            {
                NewBone.TagName = Strings::Format("no_tag_%d", i);
            }
        }

        int32_t rawParent = -1;

        if (i >= Model->RootBoneCount)
        {
            size_t ParentIDIndex = i - Model->RootBoneCount;

            switch (Model->BoneParentSize)
            {
            case 1: rawParent = (int32_t)CoDAssets::GameInstance->Read<uint8_t>(Model->BoneParentsPtr + ParentIDIndex); break;
            case 2: rawParent = (int32_t)CoDAssets::GameInstance->Read<uint16_t>(Model->BoneParentsPtr + ParentIDIndex * sizeof(uint16_t)); break;
            case 4: rawParent = (int32_t)CoDAssets::GameInstance->Read<uint32_t>(Model->BoneParentsPtr + ParentIDIndex * sizeof(uint32_t)); break;
            }

            if (i < Model->BoneCount)
                NewBone.BoneParent = (int32_t)(i - rawParent);
            else
                NewBone.IsCosmetic = true;
        }
        else
        {
            NewBone.BoneParent = -1;
        }

        printf("bone=%zu name=%s rootCount=%zu rawParent=%d finalParent=%d\n",
            i, NewBone.TagName.c_str(), (size_t)Model->RootBoneCount, rawParent, NewBone.BoneParent);

        // Read global data
        auto GlobalData = CoDAssets::GameInstance->Read<DObjAnimMat>(Model->BaseMatriciesPtr + i * sizeof(DObjAnimMat));

        // Assign global position
        NewBone.GlobalPosition = GlobalData.Translation;
        NewBone.GlobalRotation = GlobalData.Rotation;
    }

    // Check if we didn't parse any bones, if we didn't, we need can "inject" tag_origin (this is mostly for MW which has weird models
    // that are rare with thousands of bones)
    if (Model->BoneCount == 0)
    {
        // Add the new bone
        auto& NewBone = ResultModel->AddBone();
        NewBone.TagName = "tag_origin";
        NewBone.BoneParent = -1;
    }

    for (size_t i = 0; i < ResultModel->Bones.size(); i++)
    {
        auto& b = ResultModel->Bones[i];

        printf("PRE-LOCAL Bone %zu (%s)\n", i, b.TagName.c_str());
        printf("  parent = %d\n", b.BoneParent);
        printf("  global pos = %f %f %f\n", b.GlobalPosition.X, b.GlobalPosition.Y, b.GlobalPosition.Z);
    }
    // Generate locals
    ResultModel->GenerateLocalPositions(true, true);
}
