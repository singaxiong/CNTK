//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "stdafx.h"
#include "CNTKLibrary.h"
#include <numeric>

namespace CNTK 
{
    // An abstract base class at the root of the standard learners hierarchy
    // It implements most of the learner functionality, except for the actual update function,
    // and adds a few pre-/postprocessing methods (which are invoked before and after the update).
    class LearnerBase : public Learner
    {
    public:
        virtual bool Update(const std::unordered_map<Parameter, NDArrayViewPtr>& gradientValues, size_t trainingSampleCount) override final;

        virtual Dictionary Serialize() const override final;

        virtual size_t CurrentVersion() const override final { return s_serializationVersion; }

        virtual void RestoreFromCheckpoint(const Dictionary& checkpoint) override final;

        virtual void ResetSmoothedGradients() override final;

    protected:
        // allocateSmoothGradients flag specifies whether NDArrayViews for smoothed gradients can be allocated 
        // in the base class constructor (in which case they are allocated with the shapes identical to the shapes of
        // the corresponding parameters) or if the allocation should be deferred to the subclass constructor (which
        // performs allocation that is specific to the particular learner, see FSAdaGrad and RMSProp).
        LearnerBase(const std::vector<Parameter>& parameters, 
                    const LearningRateSchedule& learningRateSchedule,
                    AdditionalLearningOptions additionalOptions,
                    bool allocateSmoothGradients = true);

        virtual void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const = 0;

        std::string LearnerType() const;

        // Returns current (per-sample) learning rate.
        double LearningRate(size_t minibatchSize) const
        {
            auto learningRate = Learner::LearningRate();
            if (m_learningRateSchedule.Unit() == LearningRateSchedule::UnitType::Minibatch)
            {
                // learning rate needs to be converted to the per-sample value.
                return (minibatchSize == 0) ? 0.0 : learningRate / minibatchSize;
            }

            return learningRate;
        }

        AdditionalLearningOptions m_additionalOptions;

        std::unordered_map<Parameter, NDArrayViewPtr> m_smoothedGradientValues;

        // The following four static protected methods expose private methods of NDArrayView class
        // (which declares LearnerBase as friend class), so that they are available to subclasses.
        template <typename ElementType>
        static std::shared_ptr<const Microsoft::MSR::CNTK::Matrix<ElementType>> GetMatrix(const NDArrayViewPtr& arrayView);

        template <typename ElementType>
        static std::shared_ptr<Microsoft::MSR::CNTK::Matrix<ElementType>> GetWritableMatrix(const NDArrayViewPtr& arrayView);

        template <typename ElementType>
        static const Microsoft::MSR::CNTK::TensorView<ElementType>* GetTensorView(const NDArrayViewPtr& arrayView);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::TensorView<ElementType>* GetWritableTensorView(const NDArrayViewPtr& arrayView);

        template <typename ElementType>
        void ClipGradient(Microsoft::MSR::CNTK::Matrix<ElementType>& gradient, size_t actualMBSize) const;

        // Performs additional preprocessing before calling the update method 
        // (gradient clipping and L2 regularization depending on the additional learning parameters).
        template <typename ElementType>
        void PreProcess(const NDArrayViewPtr& parameterValue, const NDArrayViewPtr& gradientValue, size_t actualMBSize) const;

        // Performs additional postprocessing after the update method has been executed
        // (noise injection and L1 regularization specified by the additional learning parameters).
        template <typename ElementType>
        void PostProcess(const Parameter& parameter, const NDArrayViewPtr& gradientValue, size_t actualMBSize) const;

        // Returns an NDArrayView with the required shape, with the same data type as parameter value
        // and allocated on the same device.
        static NDArrayViewPtr AllocateNDArrayView(const Parameter& parameter, const NDShape& shape);

        // Retrieves the shape of the matrix corresponding to the parameter value.
        static NDShape GetMatrixShape(const Parameter& parameter);

        size_t m_minibatchCount;

    private:
        // Templatized update function, it invokes preprocess and postprocess using the provided
        // template parameter and also invokes virtual Update method implemented in one of the subclasses.
        template <typename ElementType>
        void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const;

        // TODO: make these functions friends of NDViewArray and move to Utils?
        static bool HasNan(const NDArrayViewPtr& value, const char* name);
        static void Print(const NDArrayViewPtr& value, const char* msg);

        static const size_t s_serializationVersion = 1;
    };

    // Vanilla gradient descent optimization algorithm.
    class LearnerSGD : public LearnerBase
    {
    public:
        LearnerSGD(const std::vector<Parameter>& parameters, 
                   const LearningRateSchedule& learningRateSchedule, 
                   AdditionalLearningOptions additionalOptions,
                   bool allocateSmoothGradients = true)
                   : LearnerBase(parameters, learningRateSchedule, additionalOptions, allocateSmoothGradients)
        {}

        // TODO: get rid of this as soon as NormalGrad is refactored.
        virtual double MomentumValueForMB(size_t /*minibatchSize*/) const
        {
            return 0.0;
        }

        virtual bool UseNesterovMomentum() const
        {
            return false;
        }

    protected:

        virtual void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const override;

        template <typename ElementType>
        void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const;
    };

    // SGD optimization with momentum. 
    class LearnerMomentumSGD : public LearnerSGD
    {
    public:
        LearnerMomentumSGD(const std::vector<Parameter>& parameters,
                           const LearningRateSchedule& learningRateSchedule,
                           const MomentumSchedule& momentumSchedule,
                           AdditionalLearningOptions additionalOptions,
                           bool allocateSmoothGradients = true)
                           : LearnerSGD(parameters, learningRateSchedule, additionalOptions, allocateSmoothGradients),
                           m_momentumSchedule(momentumSchedule)
        { }

        // returns current per-minibatch momentum value.
        virtual double MomentumValueForMB(size_t minibatchSize) const override
        {
            return MomentumValueForMB(m_momentumSchedule, minibatchSize);
        }

    protected:
        // returns current per-minibatch momentum value from the provided schedule.
        double MomentumValueForMB(const MomentumSchedule& schedule, size_t minibatchSize) const;

    private:
        MomentumSchedule m_momentumSchedule;
    };

    // Nesterov's accelerated SGDLearnerBase descent. 
    class LearnerNesterov : public LearnerMomentumSGD
    {
    public:

        LearnerNesterov(const std::vector<Parameter>& parameters,
                        const LearningRateSchedule& learningRateSchedule,
                        const MomentumSchedule& momentumSchedule,
                        AdditionalLearningOptions additionalOptions)
                        : LearnerMomentumSGD(parameters, learningRateSchedule, momentumSchedule, additionalOptions, /*allocateSmoothGradients*/ true)
        {}

        virtual bool UseNesterovMomentum() const override
        {
            return true;
        }
    };

    class LearnerAdaGrad : public LearnerBase
    {
    public:

        LearnerAdaGrad(const std::vector<Parameter>& parameters,
                       const LearningRateSchedule& learningRateSchedule,
                       bool needAveMultiplier,
                       AdditionalLearningOptions additionalOptions);

    protected:
        bool m_needAveMultiplier;

        virtual void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const override;

        template <typename ElementType>
        void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const;
    };

    class LearnerFSAdaGrad : public LearnerMomentumSGD
    {
    public:

        LearnerFSAdaGrad(const std::vector<Parameter>& parameters,
                         const LearningRateSchedule& learningRateSchedule,
                         const MomentumSchedule& momentumSchedule,
                         const MomentumSchedule& varianceMomentumSchedule,
                         AdditionalLearningOptions additionalOptions);

    protected:

        virtual void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const override;

        template <typename ElementType>
        void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const;

    private:
        static const double s_targetAdagradAvDenom;

        // returns current per-minibatch variance momentum value.
        double VarianceMomentumValueForMB(size_t minibatchSize) const
        {
            return MomentumValueForMB(m_varianceMomentumSchedule, minibatchSize);
        }

        mutable std::unordered_map<Parameter, double> m_smoothedCounts;
        MomentumSchedule m_varianceMomentumSchedule;
    };

    class LearnerRMSProp : public LearnerBase
    {
    public:

        LearnerRMSProp(const std::vector<Parameter>& parameters,
                       const LearningRateSchedule& learningRateSchedule,
                       double gamma, double inc, double dec, double max, double min,
                       bool needAveMultiplier,
                       AdditionalLearningOptions additionalOptions);

    protected:

        double m_gamma;
        double m_inc;
        double m_dec;
        double m_max;
        double m_min;
        bool m_needAveMultiplier;

        virtual void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const override;

        template <typename ElementType>
        void Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const;
    };
}
