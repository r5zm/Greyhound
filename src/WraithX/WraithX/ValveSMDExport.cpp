#include "stdafx.h"

#include "ValveSMDExport.h"
#include "TextWriter.h"
#include "FileSystems.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kEpsilon = 1e-6f;

    struct BoneFramePose
    {
        Vector3 LocalPosition;
        Quaternion LocalRotation;
        Vector3 GlobalPosition;
        Quaternion GlobalRotation;
    };

    struct VertexWeightLink
    {
        uint32_t BoneIndex;
        float Weight;
    };

    // ---------- Math Helpers ----------

    static Quaternion NormalizeQuat(const Quaternion& q)
    {
        float lenSq = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
        if (lenSq <= kEpsilon)
            return Quaternion(0,0,0,1);

        float inv = 1.0f / std::sqrt(lenSq);
        return Quaternion(q.X*inv, q.Y*inv, q.Z*inv, q.W*inv);
    }

    static Vector3 Lerp(const Vector3& a, const Vector3& b, float t)
    {
        return Vector3(
            a.X + (b.X - a.X) * t,
            a.Y + (b.Y - a.Y) * t,
            a.Z + (b.Z - a.Z) * t
        );
    }

    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t)
    {
        Quaternion q1 = NormalizeQuat(a);
        Quaternion q2 = NormalizeQuat(b);

        float dot = q1.X*q2.X + q1.Y*q2.Y + q1.Z*q2.Z + q1.W*q2.W;

        if (dot < 0.0f)
        {
            dot = -dot;
            q2 = Quaternion(-q2.X, -q2.Y, -q2.Z, -q2.W);
        }

        if (dot > 0.9995f)
        {
            return NormalizeQuat(Quaternion(
                q1.X + t*(q2.X - q1.X),
                q1.Y + t*(q2.Y - q1.Y),
                q1.Z + t*(q2.Z - q1.Z),
                q1.W + t*(q2.W - q1.W)
            ));
        }

        float theta = std::acos(dot);
        float sinTheta = std::sin(theta);

        float w1 = std::sin((1 - t)*theta) / sinTheta;
        float w2 = std::sin(t*theta) / sinTheta;

        return NormalizeQuat(Quaternion(
            q1.X*w1 + q2.X*w2,
            q1.Y*w1 + q2.Y*w2,
            q1.Z*w1 + q2.Z*w2,
            q1.W*w1 + q2.W*w2
        ));
    }

    static Vector3 RotateVector(const Vector3& value, const Quaternion& rotation)
    {
        Quaternion vectorQuat(value.X, value.Y, value.Z, 0.0f);
        Quaternion rotated = rotation * vectorQuat * ~rotation;
        return Vector3(rotated.X, rotated.Y, rotated.Z);
    }

    template<typename T>
    static bool GetKeyPair(const std::vector<WraithAnimFrame<T>>& keys,
                          uint32_t frame,
                          WraithAnimFrame<T>& prev,
                          WraithAnimFrame<T>& next,
                          bool& hasPrev,
                          bool& hasNext)
    {
        hasPrev = false;
        hasNext = false;

        for (const auto& k : keys)
        {
            if (k.Frame == frame)
            {
                prev = next = k;
                hasPrev = hasNext = true;
                return true;
            }

            if (k.Frame < frame)
            {
                if (!hasPrev || k.Frame > prev.Frame)
                {
                    prev = k;
                    hasPrev = true;
                }
            }
            else
            {
                if (!hasNext || k.Frame < next.Frame)
                {
                    next = k;
                    hasNext = true;
                }
            }
        }

        return false;
    }

    static Vector3 ResolvePos(const std::vector<WraithAnimFrame<Vector3>>& keys,
                              uint32_t frame,
                              const Vector3& fallback)
    {
        if (keys.empty()) return fallback;

        WraithAnimFrame<Vector3> prev, next;
        bool hasPrev, hasNext;

        if (GetKeyPair(keys, frame, prev, next, hasPrev, hasNext))
            return prev.Value;

        if (hasPrev && hasNext)
        {
            float t = float(frame - prev.Frame) / float(next.Frame - prev.Frame);
            return Lerp(prev.Value, next.Value, t);
        }

        if (hasPrev) return prev.Value;
        if (hasNext) return next.Value;

        return fallback;
    }

    static Quaternion ResolveRot(const std::vector<WraithAnimFrame<Quaternion>>& keys,
                                 uint32_t frame,
                                 const Quaternion& fallback)
    {
        if (keys.empty()) return fallback;

        WraithAnimFrame<Quaternion> prev, next;
        bool hasPrev, hasNext;

        if (GetKeyPair(keys, frame, prev, next, hasPrev, hasNext))
            return NormalizeQuat(prev.Value);

        if (hasPrev && hasNext)
        {
            float t = float(frame - prev.Frame) / float(next.Frame - prev.Frame);
            return Slerp(prev.Value, next.Value, t);
        }

        if (hasPrev) return NormalizeQuat(prev.Value);
        if (hasNext) return NormalizeQuat(next.Value);

        return fallback;
    }

    static WraithAnimationType ResolveBoneMode(const WraithAnim& animation, const WraithBone& bone)
    {
        auto modifier = animation.AnimationBoneModifiers.find(bone.TagName);
        if (modifier != animation.AnimationBoneModifiers.end())
            return modifier->second;

        return animation.AnimType;
    }

    static std::string ResolveMaterialName(const WraithModel& model, const WraithSubmesh& submesh)
    {
        if (!submesh.MaterialIndicies.empty())
        {
            int32_t materialIndex = submesh.MaterialIndicies[0];
            if (materialIndex >= 0 && materialIndex < (int32_t)model.Materials.size())
            {
                const auto& material = model.Materials[(size_t)materialIndex];
                if (!material.MaterialName.empty())
                    return material.MaterialName;
            }
        }

        return WraithMaterial::DefaultMaterial.MaterialName;
    }

    static std::vector<VertexWeightLink> ResolveVertexWeights(const WraithModel& model, const WraithVertex& vertex)
    {
        std::vector<VertexWeightLink> links;
        links.reserve(vertex.Weights.size());

        float totalWeight = 0.0f;

        for (const auto& weight : vertex.Weights)
        {
            if (weight.Weight <= 0.0f)
                continue;

            uint32_t boneIndex = weight.BoneIndex;
            if (boneIndex >= model.Bones.size())
                boneIndex = 0;

            links.push_back({ boneIndex, weight.Weight });
            totalWeight += weight.Weight;
        }

        if (links.empty())
        {
            links.push_back({ 0u, 1.0f });
            return links;
        }

        if (totalWeight > kEpsilon)
        {
            for (auto& link : links)
                link.Weight /= totalWeight;
        }
        else
        {
            links.clear();
            links.push_back({ 0u, 1.0f });
        }

        return links;
    }

    static void WriteSMDVertex(TextWriter& writer, const WraithModel& model, const WraithVertex& vertex)
    {
        auto links = ResolveVertexWeights(model, vertex);
        uint32_t parentBone = links[0].BoneIndex;
        Vector2 uv = vertex.UVLayers.empty() ? Vector2(0.0f, 0.0f) : vertex.UVLayers[0];

        writer.WriteFmt("%u %f %f %f %f %f %f %f %f %u",
            parentBone,
            vertex.Position.X, vertex.Position.Y, vertex.Position.Z,
            vertex.Normal.X, vertex.Normal.Y, vertex.Normal.Z,
            uv.U, uv.V,
            (uint32_t)links.size());

        for (const auto& link : links)
            writer.WriteFmt(" %u %f", link.BoneIndex, link.Weight);

        writer.WriteLine("");
    }

    static void WriteModelQC(const WraithModel& model, const std::string& smdFileName)
    {
        const auto smdDirectory = FileSystems::GetDirectoryName(smdFileName);
        const auto smdBaseName = FileSystems::GetFileNameWithoutExtension(smdFileName);
        const auto qcPath = FileSystems::CombinePath(smdDirectory, smdBaseName + ".qc");
        const auto smdLeafName = FileSystems::GetFileName(smdFileName);
        const auto mdlLeafName = smdBaseName + ".mdl";

        TextWriter qcWriter;
        qcWriter.Create(qcPath);

        qcWriter.WriteLineFmt("$modelname \"%s\"", mdlLeafName.c_str());
        qcWriter.WriteLine("$cdmaterials \".\"");
        qcWriter.WriteLine("$surfaceprop \"default\"");

        if (model.BoneCount() > 0)
            qcWriter.WriteLine("$mostlyopaque");

        qcWriter.WriteLineFmt("$body studio \"%s\"", smdLeafName.c_str());
        qcWriter.WriteLineFmt("$sequence idle \"%s\" fps 30", smdLeafName.c_str());
    }
}

// ---------- MODEL EXPORT (UNCHANGED) ----------

void ValveSMD::ExportSMD(const WraithModel& Model, const std::string& FileName)
{
    TextWriter writer;
    writer.Create(FileName);

    writer.WriteLine("version 1");

    writer.WriteLine("nodes");
    for (uint32_t i = 0; i < Model.Bones.size(); i++)
    {
        const auto& b = Model.Bones[i];
        writer.WriteLineFmt("%u \"%s\" %d", i, b.TagName.c_str(), b.BoneParent);
    }
    writer.WriteLine("end");

    writer.WriteLine("skeleton");
    writer.WriteLine("time 0");

    for (uint32_t i = 0; i < Model.Bones.size(); i++)
    {
        const auto& b = Model.Bones[i];
        Vector3 e = b.LocalRotation.ToEulerAngles();

        writer.WriteLineFmt("%u %f %f %f %f %f %f",
            i,
            b.LocalPosition.X, b.LocalPosition.Y, b.LocalPosition.Z,
            VectorMath::DegreesToRadians(e.X),
            VectorMath::DegreesToRadians(e.Y),
            VectorMath::DegreesToRadians(e.Z)
        );
    }

    writer.WriteLine("end");

    writer.WriteLine("triangles");
    for (const auto& submesh : Model.Submeshes)
    {
        const auto materialName = ResolveMaterialName(Model, submesh);

        for (const auto& face : submesh.Faces)
        {
            writer.WriteLine(materialName);
            WriteSMDVertex(writer, Model, submesh.Verticies[face.Index1]);
            WriteSMDVertex(writer, Model, submesh.Verticies[face.Index3]);
            WriteSMDVertex(writer, Model, submesh.Verticies[face.Index2]);
        }
    }
    writer.WriteLine("end");

    WriteModelQC(Model, FileName);
}

// ---------- LEGACY (SAFE FALLBACK) ----------

void ValveSMD::ExportSMD(const WraithAnim& Animation, const std::string& FileName)
{
    TextWriter writer;
    writer.Create(FileName);

    writer.WriteLine("version 1");
    writer.WriteLine("nodes");
    writer.WriteLine("0 \"root\" -1");
    writer.WriteLine("end");

    writer.WriteLine("skeleton");
    writer.WriteLine("time 0");
    writer.WriteLine("0 0 0 0 0 0 0");
    writer.WriteLine("end");
}

// ---------- CORRECT ANIMATION EXPORT ----------

void ValveSMD::ExportSMD(const WraithAnim& Animation,
                         const WraithModel& ReferenceModel,
                         const std::string& FileName)
{
    TextWriter writer;
    writer.Create(FileName);

    writer.WriteLine("version 1");

    // Nodes
    writer.WriteLine("nodes");
    for (uint32_t i = 0; i < ReferenceModel.Bones.size(); i++)
    {
        const auto& b = ReferenceModel.Bones[i];
        writer.WriteLineFmt("%u \"%s\" %d", i, b.TagName.c_str(), b.BoneParent);
    }
    writer.WriteLine("end");

    // Skeleton
    writer.WriteLine("skeleton");

    uint32_t frameCount = std::max<uint32_t>(Animation.FrameCount(), 1u);

    for (uint32_t f = 0; f < frameCount; f++)
    {
        writer.WriteLineFmt("time %u", f);

        std::vector<BoneFramePose> framePoses(ReferenceModel.Bones.size());

        for (uint32_t i = 0; i < ReferenceModel.Bones.size(); i++)
        {
            const auto& bone = ReferenceModel.Bones[i];
            auto& pose = framePoses[i];
            WraithAnimationType boneMode = ResolveBoneMode(Animation, bone);

            pose.LocalPosition = bone.LocalPosition;
            pose.LocalRotation = NormalizeQuat(bone.LocalRotation);
            pose.GlobalPosition = bone.GlobalPosition;
            pose.GlobalRotation = NormalizeQuat(bone.GlobalRotation);

            auto p = Animation.AnimationPositionKeys.find(bone.TagName);
            auto r = Animation.AnimationRotationKeys.find(bone.TagName);

            if (boneMode == WraithAnimationType::Absolute)
            {
                if (p != Animation.AnimationPositionKeys.end())
                    pose.GlobalPosition = ResolvePos(p->second, f, bone.GlobalPosition);

                if (r != Animation.AnimationRotationKeys.end())
                    pose.GlobalRotation = ResolveRot(r->second, f, bone.GlobalRotation);
            }
            else
            {
                if (p != Animation.AnimationPositionKeys.end())
                    pose.LocalPosition = bone.LocalPosition + ResolvePos(p->second, f, Vector3(0.0f, 0.0f, 0.0f));

                if (r != Animation.AnimationRotationKeys.end())
                {
                    Quaternion deltaRotation = ResolveRot(r->second, f, Quaternion::Identity());
                    pose.LocalRotation = NormalizeQuat(bone.LocalRotation * deltaRotation);
                }

                if (bone.BoneParent >= 0 && bone.BoneParent < (int32_t)framePoses.size())
                {
                    const auto& parentPose = framePoses[(size_t)bone.BoneParent];
                    pose.GlobalRotation = NormalizeQuat(parentPose.GlobalRotation * pose.LocalRotation);
                    pose.GlobalPosition = parentPose.GlobalPosition + RotateVector(pose.LocalPosition, parentPose.GlobalRotation);
                }
                else
                {
                    pose.GlobalPosition = pose.LocalPosition;
                    pose.GlobalRotation = pose.LocalRotation;
                }
            }

            pose.GlobalRotation = NormalizeQuat(pose.GlobalRotation);

            if (bone.BoneParent >= 0 && bone.BoneParent < (int32_t)framePoses.size())
            {
                const auto& parentPose = framePoses[(size_t)bone.BoneParent];
                pose.LocalPosition = RotateVector(pose.GlobalPosition - parentPose.GlobalPosition, ~parentPose.GlobalRotation);
                pose.LocalRotation = NormalizeQuat((~parentPose.GlobalRotation) * pose.GlobalRotation);
            }
            else
            {
                pose.LocalPosition = pose.GlobalPosition;
                pose.LocalRotation = pose.GlobalRotation;
            }

            Vector3 e = pose.LocalRotation.ToEulerAngles();

            writer.WriteLineFmt("%u %f %f %f %f %f %f",
                i,
                pose.LocalPosition.X, pose.LocalPosition.Y, pose.LocalPosition.Z,
                VectorMath::DegreesToRadians(e.X),
                VectorMath::DegreesToRadians(e.Y),
                VectorMath::DegreesToRadians(e.Z)
            );
        }
    }

    writer.WriteLine("end");
}
