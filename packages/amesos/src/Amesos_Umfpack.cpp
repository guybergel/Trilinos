// @HEADER
// ***********************************************************************
// 
//                Amesos: Direct Sparse Solver Package
//                 Copyright (2004) Sandia Corporation
// 
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
// 
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//  
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//  
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
// Questions? Contact Michael A. Heroux (maherou@sandia.gov) 
// 
// ***********************************************************************
// @HEADER

#include "Amesos_Umfpack.h"
extern "C" {
#include "umfpack.h"
}
#include "Epetra_Map.h"
#include "Epetra_Import.h"
#include "Epetra_Export.h"
#include "Epetra_RowMatrix.h"
#include "Epetra_CrsMatrix.h"
#include "Epetra_Vector.h"
#include "Epetra_Util.h"

//=============================================================================
Amesos_Umfpack::Amesos_Umfpack(const Epetra_LinearProblem &prob ) :
  IsSymbolicFactorizationOK_(false), 
  IsNumericFactorizationOK_(false), 
  Symbolic(0),
  Numeric(0),
  SerialMap_(0), 
  SerialMatrix_(0), 
  SerialCrsMatrixA_(0),
  UseTranspose_(false),
  Problem_(&prob), 
  Rcond_(0.0), 
  RcondValidOnAllProcs_(true), 
  PrintTiming_(false),
  PrintStatus_(false),
  AddToDiag_(0.0),
  ComputeVectorNorms_(false),
  ComputeTrueResidual_(false),
  verbose_(1),
  ConTime_(0.0),
  SymTime_(0.0),
  NumTime_(0.0),
  SolTime_(0.0),
  VecTime_(0.0),
  MatTime_(0.0),
  NumSymbolicFact_(0),
  NumNumericFact_(0),
  NumSolve_(0),
  Time_(0),
  ImportToSerial_(0)
{
  
  // MS // move declaration of Problem_ above because I need it
  // MS // set up before calling Comm()
  Teuchos::ParameterList ParamList ;
  SetParameters( ParamList ) ; 
}

//=============================================================================
Amesos_Umfpack::~Amesos_Umfpack(void) 
{

  if ( SerialMap_ ) delete SerialMap_ ; 
  if ( Symbolic ) umfpack_di_free_symbolic (&Symbolic) ;
  if ( Numeric ) umfpack_di_free_numeric (&Numeric) ;

    if ( SerialCrsMatrixA_ ) delete SerialCrsMatrixA_ ; 

  if( Time_ ) delete Time_;

  if( ImportToSerial_ ) { delete ImportToSerial_; ImportToSerial_ = 0; }

  // print out some information if required by the user
  if( (verbose_ && PrintTiming_) || verbose_ == 2 ) PrintTiming();
  if( (verbose_ && PrintStatus_) || verbose_ == 2 ) PrintStatus();
  
}

//=============================================================================
int Amesos_Umfpack::ConvertToSerial() 
{ 

  Time_->ResetStartTime();
  
  iam = Comm().MyPID() ;

  const Epetra_Map &OriginalMap = Matrix()->RowMatrixRowMap() ; 

  NumGlobalElements_ = Matrix()->NumGlobalRows();
  numentries_ = Matrix()->NumGlobalNonzeros();
  assert (NumGlobalElements_ == Matrix()->NumGlobalCols());

  int NumMyElements_ = 0 ;
  if (iam==0) NumMyElements_ = NumGlobalElements_;

  IsLocal_ = ( OriginalMap.NumMyElements() == 
	       OriginalMap.NumGlobalElements() )?1:0;
  Comm().Broadcast( &IsLocal_, 1, 0 ) ; 

  //  Convert Original Matrix to Serial (if it is not already) 
  //
  if (IsLocal_== 1) {
     SerialMatrix_ = Matrix();
  } 
  else {
    if (SerialMap_) { delete SerialMap_ ; SerialMap_ = 0 ; } 

    SerialMap_ = new Epetra_Map(NumGlobalElements_,NumMyElements_,0,Comm());
    assert (SerialMap_ != 0);

    Epetra_Export export_to_serial(OriginalMap,*SerialMap_);

    if ( SerialCrsMatrixA_ ) delete SerialCrsMatrixA_ ; 

    SerialCrsMatrixA_ = new Epetra_CrsMatrix(Copy,*SerialMap_,0);
    SerialCrsMatrixA_->Export(*Matrix(), export_to_serial,Insert); 
    
    SerialCrsMatrixA_->FillComplete(); 
    SerialMatrix_ = SerialCrsMatrixA_;
  }

  MatTime_ += Time_->ElapsedTime();
  
  return(0);
} 

//=============================================================================
int Amesos_Umfpack::ConvertToUmfpackCRS()
{
  
  Time_->ResetStartTime();
  
  //  Convert matrix to the form that Umfpack expects (Ap, Ai, Aval) 

  assert( NumGlobalElements_ == SerialMatrix_->NumGlobalRows());
  assert( NumGlobalElements_ == SerialMatrix_->NumGlobalCols());
  assert( numentries_ == SerialMatrix_->NumGlobalNonzeros());
  Ap.resize( NumGlobalElements_+1 );
  Ai.resize( EPETRA_MAX( NumGlobalElements_, numentries_) ) ; 
  Aval.resize( EPETRA_MAX( NumGlobalElements_, numentries_) ) ; 

  int NumEntries = SerialMatrix_->MaxNumEntries();
  vector<int> Indices;
  vector<double> Values;
  Indices.resize(NumEntries);
  Values.resize(NumEntries);
  
  if (iam == 0) {

    int NumEntriesThisRow;
    int Ai_index = 0 ; 
    int MyRow;
    for (MyRow = 0 ; MyRow < NumGlobalElements_; MyRow++) {

      int ierr;
      ierr = SerialMatrix_->ExtractMyRowCopy(MyRow, NumEntries, 
					     NumEntriesThisRow, 
					     &Values[0], &Indices[0]);
      if (ierr)
	AMESOS_CHK_ERR(-1);

      // MS // added on 15-Mar-05
      if (AddToDiag_ != 0.0) {
        for (int i = 0 ; i < NumEntriesThisRow ; ++i) {
          if (Indices[i] == MyRow) {
            Values[i] += AddToDiag_;
            break;
          }
        }
      }

      Ap[MyRow] = Ai_index ; 
      for ( int j = 0; j < NumEntriesThisRow; j++ ) { 
	Ai[Ai_index] = Indices[j] ; 
	Aval[Ai_index] = Values[j] ; 
	Ai_index++;
      }
    }
    Ap[MyRow] = Ai_index ; 
  }

  ConTime_ += Time_->ElapsedTime();
  
  return 0;
}   

//=============================================================================
int Amesos_Umfpack::SetParameters( Teuchos::ParameterList &ParameterList ) {

  //  Some compilers reject the following cast:
  //  if(  (int) &ParameterList == 0 ) return 0;

  // ========================================= //
  // retrive UMFPACK's parameters from list.   //
  // default values defined in the constructor //
  // ========================================= //
  
  // retrive general parameters

  // solve problem with transpose
  if( ParameterList.isParameter("UseTranspose") )
    SetUseTranspose(ParameterList.get("UseTranspose",false));

  // print some timing information (on process 0)
  if( ParameterList.isParameter("PrintTiming") )
    PrintTiming_ = ParameterList.get("PrintTiming", false);

  // print some statistics (on process 0). Do not include timing
  if( ParameterList.isParameter("PrintStatus") )
    PrintStatus_ = ParameterList.get("PrintStatus", false);

  // add this value to diagonal
  if( ParameterList.isParameter("AddToDiag") )
    AddToDiag_ = ParameterList.get("AddToDiag", 0.0);

  // compute norms of some vectors
  if( ParameterList.isParameter("ComputeVectorNorms") )
    ComputeVectorNorms_ = ParameterList.get("ComputeVectorNorms",false);

  // compute the true residual Ax-b after solution
  if( ParameterList.isParameter("ComputeTrueResidual") )
    ComputeTrueResidual_ = ParameterList.get("ComputeTrueResidual",false);

  // some verbose output:
  // 0 - no output at all
  // 1 - output as specified by other parameters
  // 2 - all possible output
  if( ParameterList.isParameter("OutputLevel") )
    verbose_ = ParameterList.get("OutputLevel",1);

  // MS // now comment it out (only because the list if empty).
  // MS // When we will have parameters for UMFPACK sublist
  // MS // uncomment it
  /*  
  if (ParameterList.isSublist("Umfpack") ) {
    Teuchos::ParameterList UmfpackParams = ParameterList.sublist("Umfpack") ;
  }
  */
  return 0;
}

//=============================================================================
int Amesos_Umfpack::PerformSymbolicFactorization() {

  Time_->ResetStartTime();  

  double *Control = (double *) NULL, *Info = (double *) NULL ;
  
  if (Symbolic) 
    umfpack_di_free_symbolic (&Symbolic) ;
  if (iam== 0) {
    (void) umfpack_di_symbolic (NumGlobalElements_, NumGlobalElements_, &Ap[0], 
				&Ai[0], &Aval[0], 
				&Symbolic, Control, Info) ;
  }

  SymTime_ += Time_->ElapsedTime();

  return 0;
}

//=============================================================================
int Amesos_Umfpack::PerformNumericFactorization( ) {

  Time_->ResetStartTime();

  RcondValidOnAllProcs_ = false ; 
  if (iam == 0) {
    vector<double> Control(UMFPACK_CONTROL);
    vector<double> Info(UMFPACK_INFO);
    umfpack_di_defaults( &Control[0] ) ; 
    if (Numeric) umfpack_di_free_numeric (&Numeric) ;
    int status = umfpack_di_numeric (&Ap[0], 
				     &Ai[0], 
				     &Aval[0], 
				     Symbolic, 
				     &Numeric, 
				     &Control[0], 
				     &Info[0]) ;
    Rcond_ = Info[UMFPACK_RCOND]; 

#if NOT_DEF
    cout << " Rcond_ = " << Rcond_ << endl ; 

    int lnz1 = 1000 ;
    int unz1 = 1000 ;
    int n = 4;
    int * Lp = (int *) malloc ((n+1) * sizeof (int)) ;
    int * Lj = (int *) malloc (lnz1 * sizeof (int)) ;
    double * Lx = (double *) malloc (lnz1 * sizeof (double)) ;
    int * Up = (int *) malloc ((n+1) * sizeof (int)) ;
    int * Ui = (int *) malloc (unz1 * sizeof (int)) ;
    double * Ux = (double *) malloc (unz1 * sizeof (double)) ;
    int * P = (int *) malloc (n * sizeof (int)) ;
    int * Q = (int *) malloc (n * sizeof (int)) ;
    double * Dx = (double *) NULL ;	/* D vector not requested */
    double * Rs  = (double *) malloc (n * sizeof (double)) ;
    if (!Lp || !Lj || !Lx || !Up || !Ui || !Ux || !P || !Q || !Rs)
    {
      assert( false ) ; 
    }
    int do_recip;
    status = umfpack_di_get_numeric (Lp, Lj, Lx, Up, Ui, Ux,
	P, Q, Dx, &do_recip, Rs, Numeric) ;
    if (status < 0)
    {
      assert( false ) ; 
    }

    printf ("\nL (lower triangular factor of C): ") ;
    (void) umfpack_di_report_matrix (n, n, Lp, Lj, Lx, 0, &Control[0]) ;
    printf ("\nU (upper triangular factor of C): ") ;
    (void) umfpack_di_report_matrix (n, n, Up, Ui, Ux, 1, &Control[0]) ;
    printf ("\nP: ") ;
    (void) umfpack_di_report_perm (n, P, &Control[0]) ;
    printf ("\nQ: ") ;
    (void) umfpack_di_report_perm (n, Q, &Control[0]) ;
    printf ("\nScale factors: row i of A is to be ") ;

#endif

    assert( status == 0 ) ; 
  }
  
  NumTime_ += Time_->ElapsedTime();
  return 0;
}

//=============================================================================
double Amesos_Umfpack::GetRcond() const {
  if ( !RcondValidOnAllProcs_ ) {
    Comm().Broadcast( &Rcond_, 1, 0 ) ; 
    RcondValidOnAllProcs_ = true; 
  }
  return(Rcond_);
}; 

//=============================================================================
bool Amesos_Umfpack::MatrixShapeOK() const 
{ 
  bool OK = true;

  if ( GetProblem()->GetOperator()->OperatorRangeMap().NumGlobalPoints() != 
       GetProblem()->GetOperator()->OperatorDomainMap().NumGlobalPoints() ) OK = false;
  return OK; 
}


//=============================================================================
int Amesos_Umfpack::SymbolicFactorization() 
{

  IsSymbolicFactorizationOK_ = false;
  IsNumericFactorizationOK_ = false;

  if (Time_ == 0) 
    Time_ = new Epetra_Time(Comm());

  NumSymbolicFact_++;  

  ConvertToSerial() ; 
  ConvertToUmfpackCRS();
  
  PerformSymbolicFactorization();

  IsSymbolicFactorizationOK_ = false; 
  return 0;
}

//=============================================================================
int Amesos_Umfpack::NumericFactorization() 
{

  IsNumericFactorizationOK_ = false;
  if( Time_ == 0 ) 
    Time_ = new Epetra_Time( Comm() );
  
  NumNumericFact_++;  

  ConvertToSerial() ; 
  
  ConvertToUmfpackCRS();
  
  if (!IsSymbolicFactorizationOK_) {
    SymbolicFactorization();
  }

  PerformNumericFactorization();

  IsNumericFactorizationOK_ = true;
  return 0;
}

//=============================================================================
int Amesos_Umfpack::Solve() 
{ 

  if( Time_ == 0 ) Time_ = new Epetra_Time( Comm() );
  
  NumSolve_++;

  // if necessary, perform numeric factorization. 
  // This may call SymbolicFactorization() as well.
  if (!IsNumericFactorizationOK_)
    AMESOS_CHK_ERR(NumericFactorization()); 

  Epetra_MultiVector* vecX = Problem_->GetLHS(); 
  Epetra_MultiVector* vecB = Problem_->GetRHS(); 

  if ((vecX == 0) || (vecB == 0))
    AMESOS_CHK_ERR(-1);

  int NumVectors = vecX->NumVectors(); 
  if (NumVectors != vecB->NumVectors())
    AMESOS_CHK_ERR(-1);

  Epetra_MultiVector *SerialB, *SerialX; 

  //  Extract Serial versions of X and B 
  //
  double *SerialXvalues ;
  double *SerialBvalues ;

  Epetra_MultiVector* SerialXextract = 0;
  Epetra_MultiVector* SerialBextract = 0;
    
  //  Copy B to the serial version of B
  //
  Time_->ResetStartTime(); // track time to broadcast vectors
  
  if (IsLocal_ == 1) { 
    SerialB = vecB ; 
    SerialX = vecX ; 
  } else { 
    assert (IsLocal_ == 0);
    const Epetra_Map& OriginalMap = Matrix()->RowMatrixRowMap();
    SerialXextract = new Epetra_MultiVector(*SerialMap_,NumVectors); 
    SerialBextract = new Epetra_MultiVector(*SerialMap_,NumVectors); 

    if (ImportToSerial_ == 0) {
      ImportToSerial_ = new Epetra_Import (*SerialMap_,OriginalMap );
    }
     
    if (!ImportToSerial_->SourceMap().SameAs(Matrix()->RowMatrixRowMap())) {
      delete SerialMap_;
      SerialMap_ = 0;
      delete ImportToSerial_;
      ImportToSerial_ = 0;
      int i = Matrix()->NumGlobalRows();
      if (Comm().MyPID())
	i = 0;
      SerialMap_ = new Epetra_Map(-1,i,0,Comm());
      ImportToSerial_ = new Epetra_Import (*SerialMap_,OriginalMap );
    }

    SerialBextract->Import(*vecB,*ImportToSerial_,Insert);
    SerialB = SerialBextract; 
    SerialX = SerialXextract; 

  } 

  VecTime_ += Time_->ElapsedTime();
  
  //  Call UMFPACK to perform the solve
  //  Note:  UMFPACK uses a Compressed Column Storage instead of compressed row storage, 
  //  Hence to compute A X = B, we ask UMFPACK to perform A^T X = B and vice versa

  Time_->ResetStartTime(); // track time to solve

  int SerialBlda, SerialXlda ; 
  int UmfpackRequest = UseTranspose()?UMFPACK_A:UMFPACK_At ;
  if ( iam == 0 ) {
    int ierr;
    ierr = SerialB->ExtractView(&SerialBvalues, &SerialBlda);
    assert (ierr == 0);
    ierr = SerialX->ExtractView(&SerialXvalues, &SerialXlda);
    assert (ierr == 0);
    assert( SerialBlda == NumGlobalElements_ ) ; 
    assert( SerialXlda == NumGlobalElements_ ) ; 
    
    for ( int j =0 ; j < NumVectors; j++ ) { 
      double *Control = (double *) NULL, *Info = (double *) NULL ;


      int status = umfpack_di_solve (UmfpackRequest, &Ap[0], 
				     &Ai[0], &Aval[0], 
				     &SerialXvalues[j*SerialXlda], 
				     &SerialBvalues[j*SerialBlda], 
				     Numeric, Control, Info) ;
      AMESOS_CHK_ERR(status); 
    }
  }
    
  SolTime_ += Time_->ElapsedTime();
  
  //  Copy X back to the original vector
  // 
  Time_->ResetStartTime();  // track time to broadcast vectors

  if ( IsLocal_ == 0 ) {
    vecX->Export( *SerialX, *ImportToSerial_, Insert ) ;
    if (SerialBextract) delete SerialBextract ;
    if (SerialXextract) delete SerialXextract ;
  }

  VecTime_ += Time_->ElapsedTime();

  // MS // compute vector norms
  if( ComputeVectorNorms_ == true || verbose_ == 2 ) {
    double NormLHS, NormRHS;
    for( int i=0 ; i<NumVectors ; ++i ) {
      (*vecX)(i)->Norm2(&NormLHS);
      (*vecB)(i)->Norm2(&NormRHS);
      if( verbose_ && Comm().MyPID() == 0 ) {
	cout << "Amesos_Umfpack : vector " << i << ", ||x|| = " << NormLHS
	     << ", ||b|| = " << NormRHS << endl;
      }
    }
  }
  
  // MS // compute true residual
  if( ComputeTrueResidual_ == true || verbose_ == 2  ) {
    double Norm;
    Epetra_MultiVector Ax(vecB->Map(),NumVectors);
    for( int i=0 ; i<NumVectors ; ++i ) {
      (Problem_->GetMatrix()->Multiply(UseTranspose(), *((*vecX)(i)), Ax));
      (Ax.Update(1.0, *((*vecB)(i)), -1.0));
      (Ax.Norm2(&Norm));
      
      if( verbose_ && Comm().MyPID() == 0 ) {
	cout << "Amesos_Umfpack : vector " << i << ", ||Ax - b|| = " << Norm << endl;
      }
    }
  }

  return(0) ; 
}

// ================================================ ====== ==== ==== == =

void Amesos_Umfpack::PrintStatus() 
{

  if( iam != 0  ) return;

  cout << "----------------------------------------------------------------------------" << endl;
  cout << "Amesos_Umfpack : Matrix has " << NumGlobalElements_ << " rows"
       << " and " << numentries_ << " nonzeros" << endl;
  cout << "Amesos_Umfpack : Nonzero elements per row = "
       << 1.0*numentries_/NumGlobalElements_ << endl;
  cout << "Amesos_Umfpack : Percentage of nonzero elements = "
       << 100.0*numentries_/(pow(NumGlobalElements_,2.0)) << endl;
  cout << "Amesos_Umfpack : Use transpose = " << UseTranspose_ << endl;
  cout << "----------------------------------------------------------------------------" << endl;

  return;
  
}

// ================================================ ====== ==== ==== == =

void Amesos_Umfpack::PrintTiming()
{
  if (iam) return;
  
  double SymTime = 0.0, NumTime = 0.0, SolTime = 0.0;

  if (NumSymbolicFact_)
    SymTime = SymTime_ / NumSymbolicFact_;

  if (NumNumericFact_)
    NumTime =  NumTime_ / NumNumericFact_;

  if (NumSolve_)
    SolTime = SolTime_ / NumSolve_;

  cout << "----------------------------------------------------------------------------" << endl;
  cout << "Amesos_Umfpack : Time to convert matrix to UMFPACK format = "
       << ConTime_ << " (s)" << endl;
  cout << "Amesos_Umfpack : Time to redistribute matrix = "
       << MatTime_ << " (s)" << endl;
  cout << "Amesos_Umfpack : Time to redistribute vectors = "
       << VecTime_ << " (s)" << endl;
  cout << "Amesos_Umfpack : Number of symbolic factorizations = "
       << NumSymbolicFact_ << endl;
  cout << "Amesos_Umfpack : Time for sym fact = "
       << SymTime_ << " (s), avg = " << SymTime << " (s)" << endl;
  cout << "Amesos_Umfpack : Number of numeric factorizations = "
       << NumNumericFact_ << endl;
  cout << "Amesos_Umfpack : Time for num fact = "
       << NumTime_ << " (s), avg = " << NumTime << " (s)" << endl;
  cout << "Amesos_Umfpack : Number of solve phases = "
       << NumSolve_ << endl;
  cout << "Amesos_Umfpack : Time for solve = "
       << SolTime_ << " (s), avg = " << SolTime << " (s)" << endl;
  cout << "----------------------------------------------------------------------------" << endl;
   
  return;
}
