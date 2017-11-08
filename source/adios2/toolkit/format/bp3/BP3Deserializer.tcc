/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BP3Deserializer.tcc
 *
 *  Created on: Sep 7, 2017
 *      Author: William F Godoy godoywf@ornl.gov
 */

#ifndef ADIOS2_TOOLKIT_FORMAT_BP1_BP3DESERIALIZER_TCC_
#define ADIOS2_TOOLKIT_FORMAT_BP1_BP3DESERIALIZER_TCC_

#include "BP3Deserializer.h"

#include "adios2/helper/adiosFunctions.h"

namespace adios2
{
namespace format
{

template <class T>
std::map<std::string, SubFileInfoMap>
BP3Deserializer::GetSyncVariableSubFileInfo(const Variable<T> &variable) const
{
    std::map<std::string, SubFileInfoMap> variableSubFileInfo;
    variableSubFileInfo[variable.m_Name] = GetSubFileInfo(variable);
    return variableSubFileInfo;
}

template <class T>
void BP3Deserializer::GetDeferredVariable(Variable<T> &variable, T *data)
{
    variable.SetData(data);
    m_DeferredVariables[variable.m_Name] = SubFileInfoMap();
}

// PRIVATE
template <class T>
void BP3Deserializer::DefineVariableInIO(const ElementIndexHeader &header,
                                         IO &io,
                                         const std::vector<char> &buffer,
                                         size_t position) const
{
    const size_t initialPosition = position;

    const Characteristics<T> characteristics =
        ReadElementIndexCharacteristics<T>(buffer, position);

    std::string variableName(header.Name);
    if (!header.Path.empty())
    {
        variableName = header.Path + PathSeparator + header.Name;
    }

    Variable<T> *variable = nullptr;
    if (characteristics.Statistics.IsValue)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        variable = &io.DefineVariable<T>(variableName);
        variable->m_Min = characteristics.Statistics.Value;
        variable->m_Max = characteristics.Statistics.Value;
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        variable =
            &io.DefineVariable<T>(variableName, characteristics.Shape,
                                  characteristics.Start, characteristics.Count);

        variable->m_Min = characteristics.Statistics.Min;
        variable->m_Max = characteristics.Statistics.Max;
    }

    // going back to get variable index position
    variable->m_IndexStart =
        initialPosition - (header.Name.size() + header.GroupName.size() +
                           header.Path.size() + 23);

    const size_t endPosition =
        variable->m_IndexStart + static_cast<size_t>(header.Length) + 4;

    position = initialPosition;

    size_t currentStep = 1;

    std::vector<size_t> subsetPositions; // per step
    subsetPositions.reserve(1);          // expecting one subset per step

    while (position < endPosition)
    {
        const size_t subsetPosition = position;

        // read until step is found
        const Characteristics<typename TypeInfo<T>::ValueType>
            subsetCharacteristics = ReadElementIndexCharacteristics<
                typename TypeInfo<T>::ValueType>(buffer, position, false);

        if (subsetCharacteristics.Statistics.Step > currentStep)
        {
            currentStep = subsetCharacteristics.Statistics.Step;
            variable->m_IndexStepBlockStarts[currentStep] = subsetPositions;
            ++variable->m_AvailableStepsCount;
            subsetPositions.clear();
        }

        if (subsetCharacteristics.Statistics.Min < variable->m_Min)
        {
            variable->m_Min = subsetCharacteristics.Statistics.Min;
        }

        if (subsetCharacteristics.Statistics.Max > variable->m_Max)
        {
            variable->m_Max = subsetCharacteristics.Statistics.Max;
        }

        subsetPositions.push_back(subsetPosition);
        position = subsetPosition + subsetCharacteristics.EntryLength + 5;

        if (position == endPosition) // check if last one
        {
            variable->m_IndexStepBlockStarts[currentStep] = subsetPositions;
            break;
        }
    }
}

template <class T>
SubFileInfoMap
BP3Deserializer::GetSubFileInfo(const Variable<T> &variable) const
{
    SubFileInfoMap infoMap;

    const auto &buffer = m_Metadata.m_Buffer;

    const size_t stepStart = variable.m_StepsStart;
    const size_t stepEnd =
        stepStart + variable.m_StepsCount; // inclusive or exclusive?

    // selection = [start, end[
    const Box<Dims> selectionBox =
        StartEndBox(variable.m_Start, variable.m_Count);

    for (size_t step = stepStart; step < stepEnd; ++step)
    {
        auto itBlockStarts = variable.m_IndexStepBlockStarts.find(step);
        if (itBlockStarts == variable.m_IndexStepBlockStarts.end())
        {
            continue;
        }

        const std::vector<size_t> &blockStarts = itBlockStarts->second;

        // blockPosition gets updated by Read, can't be const
        for (size_t blockPosition : blockStarts)
        {
            const Characteristics<T> blockCharacteristics =
                ReadElementIndexCharacteristics<T>(buffer, blockPosition);

            // check if they intersect
            SubFileInfo info;
            info.BlockBox = StartEndBox(blockCharacteristics.Start,
                                        blockCharacteristics.Count);
            info.IntersectionBox = IntersectionBox(selectionBox, info.BlockBox);

            if (info.IntersectionBox.first.empty() ||
                info.IntersectionBox.second.empty())
            {
                continue;
            }
            // if they intersect get info Seeks (first: start, second: count)
            info.Seeks.first =
                blockCharacteristics.Statistics.PayloadOffset +
                LinearIndex(info.BlockBox, info.IntersectionBox.first,
                            m_IsRowMajor, m_IsZeroIndex) *
                    sizeof(T);

            info.Seeks.second =
                blockCharacteristics.Statistics.PayloadOffset +
                (LinearIndex(info.BlockBox, info.IntersectionBox.second,
                             m_IsRowMajor, m_IsZeroIndex) +
                 1) *
                    sizeof(T);

            const size_t fileIndex = static_cast<const size_t>(
                blockCharacteristics.Statistics.FileIndex);

            infoMap[fileIndex][step].push_back(std::move(info));
        }
    }

    return infoMap;
}

template <class T>
void BP3Deserializer::ClipContiguousMemoryCommon(
    Variable<T> &variable, const std::vector<char> &contiguousMemory,
    const Box<Dims> &blockBox, const Box<Dims> &intersectionBox) const
{
    const Dims &start = intersectionBox.first;
    if (start.size() == 1) // 1D copy memory
    {
        // normalize intersection start with variable.m_Start
        const size_t normalizedStart =
            (start[0] - variable.m_Start[0]) * sizeof(T);
        char *rawVariableData = reinterpret_cast<char *>(variable.GetData());

        std::copy(contiguousMemory.begin(), contiguousMemory.end(),
                  &rawVariableData[normalizedStart]);

        return;
    }

    if (m_IsRowMajor && m_IsZeroIndex)
    {
        ClipContiguousMemoryCommonRowZero(variable, contiguousMemory, blockBox,
                                          intersectionBox);
    }
}

template <class T>
void BP3Deserializer::ClipContiguousMemoryCommonRowZero(
    Variable<T> &variable, const std::vector<char> &contiguousMemory,
    const Box<Dims> &blockBox, const Box<Dims> &intersectionBox) const
{
    const Dims &start = intersectionBox.first;
    const Dims &end = intersectionBox.second;
    const size_t stride = (end.back() - start.back() + 1) * sizeof(T);

    Dims currentPoint(start); // current point for memory copy

    const Box<Dims> selectionBox =
        StartEndBox(variable.m_Start, variable.m_Count);

    const size_t dimensions = start.size();
    bool run = true;

    const size_t intersectionStart =
        LinearIndex(blockBox, intersectionBox.first, true, true) * sizeof(T);

    while (run)
    {
        // here copy current linear memory between currentPoint and end
        const size_t contiguousStart =
            LinearIndex(blockBox, currentPoint, true, true) * sizeof(T) -
            intersectionStart;

        const size_t variableStart =
            LinearIndex(selectionBox, currentPoint, true, true) * sizeof(T);

        char *rawVariableData = reinterpret_cast<char *>(variable.GetData());

        std::copy(&contiguousMemory[contiguousStart],
                  &contiguousMemory[contiguousStart + stride],
                  &rawVariableData[variableStart]);

        // here update each index recursively, always starting from the 2nd
        // fastest changing index, since fastest changing index is the
        // continuous part in the previous std::copy
        size_t p = dimensions - 2;
        while (true)
        {
            ++currentPoint[p];
            if (currentPoint[p] > end[p]) // TODO: check end condition
            {
                if (p == 0)
                {
                    run = false; // we are done
                    break;
                }
                else
                {
                    currentPoint[p] = start[p];
                    --p;
                }
            }
            else
            {
                break; // break inner p loop
            }
        } // dimension index update
    }     // run
}

} // end namespace format
} // end namespace adios2

#endif /* ADIOS2_TOOLKIT_FORMAT_BP1_BP3DESERIALIZER_TCC_ */