/*---------------------------------------------------------------------------*\
    CFDEMcoupling - Open Source CFD-DEM coupling

    CFDEMcoupling is part of the CFDEMproject
    www.cfdem.com
                                Christoph Goniva, christoph.goniva@cfdem.com
                                Copyright 2009-2012 JKU Linz
                                Copyright 2012-2015 DCS Computing GmbH, Linz
                                Copyright 2015-     JKU Linz
-------------------------------------------------------------------------------
License
    This file is part of CFDEMcoupling.

    CFDEMcoupling is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 3 of the License, or (at your
    option) any later version.

    CFDEMcoupling is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with CFDEMcoupling; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

Description
    This code is designed to realize coupled CFD-DEM simulations using LIGGGHTS
    and OpenFOAM(R). Note: this code is not part of OpenFOAM(R) (see DISCLAIMER).

    This function is based on the derivation in R. Mei,
    An approximate expression for shear lift force on a spherical  particle at a
    finite Reynolds number,
    Int. J. Multiph. Flow 18 (1992) 145–147

    The data for this functions is based on J.B. Mclaughlin,
    Inertial migration of a small sphere in linear shear flows,
    Journal of Fluid Mechanics. 224 (1991) 261-274.

    The second order terms are based on E. Loth and A. J. Dorgan,
    An equation of motion for particles of finite Reynolds number and size,
    Environ. Fluid Mech. 9 (2009) 187–206
    and can be added to the lift coefficient if desired
\*---------------------------------------------------------------------------*/

#include "error.H"

#include "MeiLift.H"
#include "addToRunTimeSelectionTable.H"

//#include <mpi.h>

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

defineTypeNameAndDebug(MeiLift, 0);

addToRunTimeSelectionTable
(
    forceModel,
    MeiLift,
    dictionary
);


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

// Construct from components
MeiLift::MeiLift
(
    const dictionary& dict,
    cfdemCloud& sm
)
:
    forceModel(dict,sm),
    propsDict_(dict.subDict(typeName + "Props")),
    velFieldName_(propsDict_.lookup("velFieldName")),
    U_(sm.mesh().lookupObject<volVectorField> (velFieldName_)),
    useSecondOrderTerms_(false)
{
    if (propsDict_.found("useSecondOrderTerms")) useSecondOrderTerms_=true;

    // init force sub model
    setForceSubModels(propsDict_);
    // define switches which can be read from dict
    forceSubM(0).setSwitchesList(SW_TREAT_FORCE_EXPLICIT,true); // activate treatExplicit switch
    forceSubM(0).setSwitchesList(SW_VERBOSE,true); // activate search for verbose switch
    forceSubM(0).setSwitchesList(SW_INTERPOLATION,true); // activate search for interpolate switch
    forceSubM(0).setSwitchesList(SW_SCALAR_VISCOSITY,true); // activate scalarViscosity switch
    forceSubM(0).readSwitches();

    particleCloud_.checkCG(false);

    //Append the field names to be probed
    particleCloud_.probeM().initialize(typeName, typeName+".logDat");
    particleCloud_.probeM().vectorFields_.append("liftForce"); //first entry must the be the force
    particleCloud_.probeM().vectorFields_.append("Urel");      //other are debug
    particleCloud_.probeM().vectorFields_.append("vorticity"); //other are debug
    particleCloud_.probeM().scalarFields_.append("Rep");       //other are debug
    particleCloud_.probeM().scalarFields_.append("Rew");       //other are debug
    particleCloud_.probeM().scalarFields_.append("J_star");    //other are debug
    particleCloud_.probeM().writeHeader();
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

MeiLift::~MeiLift()
{
}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void MeiLift::setForce() const
{
    const volScalarField& nufField = forceSubM(0).nuField();
    const volScalarField& rhoField = forceSubM(0).rhoField();

    vector position(0,0,0);
    vector lift(0,0,0);
    vector Us(0,0,0);
    vector Ur(0,0,0);
    scalar magUr(0);
    scalar magVorticity(0);
    scalar ds(0);
    scalar nuf(0);
    scalar rho(0);
    scalar voidfraction(1);
    scalar Rep(0);
    scalar Rew(0);
    scalar Cl(0);
    scalar Cl_star(0);
    scalar J_star(0);
    scalar Omega_eq(0);
    scalar alphaStar(0);
    scalar epsilonSqr(0.0);
    scalar epsilon(0);
    scalar omega_star(0);
    vector vorticity(0,0,0);
    volVectorField vorticityField = fvc::curl(U_);

    interpolationCellPoint<vector> UInterpolator_(U_);
    interpolationCellPoint<vector> VorticityInterpolator_(vorticityField);

    #include "setupProbeModel.H"

    for (int index = 0; index < particleCloud_.numberOfParticles(); ++index)
    {
        //if(mask[index][0])
        //{
            lift = vector::zero;
            label cellI = particleCloud_.cellIDs()[index][0];

            if (cellI > -1) // particle Found
            {
                Us = particleCloud_.velocity(index);

                if (forceSubM(0).interpolation())
                {
                    position  = particleCloud_.position(index);
                    Ur        = UInterpolator_.interpolate(position,cellI) - Us;
                    vorticity = VorticityInterpolator_.interpolate(position,cellI);
                }
                else
                {
                    Ur        = U_[cellI] - Us;
                    vorticity = vorticityField[cellI];
                }

                magUr        = mag(Ur);
                magVorticity = mag(vorticity);

                if (magUr > 0 && magVorticity > 0)
                {
                    ds  = 2. * particleCloud_.radius(index);
                    nuf = nufField[cellI];
                    rho = rhoField[cellI];

                    // calc dimensionless properties
                    Rep = ds*magUr/nuf;
                    Rew = magVorticity*ds*ds/nuf;

                    omega_star = magVorticity * ds / magUr;
                    alphaStar = 0.5 * omega_star;
                    epsilonSqr = omega_star / Rep;
                    epsilon = sqrt(epsilonSqr);

                    //Basic model for the correction to the Saffman lift
                    //McLaughlin (1991), Mei (1992), Loth and Dorgan (2009)
                    //J_star = 0.443 * J
                    if (epsilon < 0.1) //epsilon << 1
                    {
                        //McLaughlin (1991), Eq (3.27): J = 32 * pi^2 * epsilon^5 * ln(1 / epsilon^2)
                        J_star = -140.0 * epsilonSqr * epsilonSqr * epsilon * log(1. / (epsilonSqr+SMALL));
                    }
                    else if (epsilon > 20.0) //epsilon >> 1
                    {
                        //McLaughlin (1991), Eq (3.26): J = 2.255 - 0.6463 / epsilon^2
                        J_star = 1.0 - 0.287 / epsilonSqr;
                    }
                    else
                    {
                        //Mei (1992), Eq (10)
                        //Loth and Dorgan (2009), Eq (32)
                        J_star = 0.3
                                 * (1.0   + tanh(2.5 * (log10(epsilon) + 0.191)))
                                 * (0.667 + tanh(6.0 * (      epsilon  - 0.32 )));
                    }
                    //Loth and Dorgan (2009), Eq (31): Saffman lift-coefficient: ClSaff = 12.92 / pi * epsilon ~ 4.11 * epsilon
                    //Loth and Dorgan (2009), Eq (32)
                    Cl = J_star * 4.11 * epsilon; //multiply correction to the basic Saffman model

                    //Second order terms given by Loth and Dorgan (2009)
                    if (useSecondOrderTerms_)
                    {
                        scalar sqrtRep = sqrt(Rep);
                        //Loth and Dorgan (2009), Eq (34)
                        Cl_star = 1.0 - (0.675 + 0.15 * (1.0 + tanh(0.28 * (alphaStar - 2.0)))) * tanh(0.18 * sqrtRep);
                        //Loth and Dorgan (2009), Eq (38)
                        Omega_eq = alphaStar * (1.0 - 0.0075 * Rew) * (1.0 - 0.062 * sqrtRep - 0.001 * Rep);
                        //Loth and Dorgan (2009), Eq (39)
                        Cl += Omega_eq * Cl_star;
                    }

                    //Loth and Dorgan (2009), Eq (27)
                    lift = 0.125 * constant::mathematical::pi
                           * rho
                           * Cl
                           * magUr * Ur ^ vorticity / magVorticity
                           * ds * ds;

                    if (modelType_ == "B")
                    {
                        voidfraction = particleCloud_.voidfraction(index);
                        lift /= voidfraction;
                    }
                }

                //**********************************
                //SAMPLING AND VERBOSE OUTOUT
                if ( forceSubM(0).verbose() )
                {
                    Pout << "index = " << index << endl;
                    Pout << "Us = " << Us << endl;
                    Pout << "Ur = " << Ur << endl;
                    Pout << "vorticity = " << vorticity << endl;
                    Pout << "ds = " << ds << endl;
                    Pout << "rho = " << rho << endl;
                    Pout << "nuf = " << nuf << endl;
                    Pout << "Rep = " << Rep << endl;
                    Pout << "Rew = " << Rew << endl;
                    Pout << "alphaStar = " << alphaStar << endl;
                    Pout << "epsilon = " << epsilon << endl;
                    Pout << "J_star = " << J_star << endl;
                    Pout << "lift = " << lift << endl;
                }

                //Set value fields and write the probe
                if (probeIt_)
                {
                    #include "setupProbeModelfields.H"
                    vValues.append(lift);   //first entry must the be the force
                    vValues.append(Ur);
                    vValues.append(vorticity);
                    sValues.append(Rep);
                    sValues.append(Rew);
                    sValues.append(J_star);
                    particleCloud_.probeM().writeProbe(index, sValues, vValues);
                }
                // END OF SAMPLING AND VERBOSE OUTOUT
                //**********************************

            }
            // write particle based data to global array
            forceSubM(0).partToArray(index,lift,vector::zero);
        //}
    }

}

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace Foam

// ************************************************************************* //
