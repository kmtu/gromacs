#include <string.h>
#include "types/mdatom.h"
#include "types/topology.h"
#include "types/idef.h"
#include "hackblock.h"
#include "types/qhoprec.h"
#include "types/gmx_qhop_types.h"
#include "gmx_fatal.h"
#include "smalloc.h"
#include "types/commrec.h"

extern int find_inert_atomtype(const gmx_mtop_t *mtop, const t_forcerec *fr)
{
  int n, i, nvdwparam, nti, ti;
  real *vdwparam, c[3];

  /* Since we ran grompp with -nurenum we know that
   * the last atomtype is the inert hydrogen.
   * This may change in future versions, however. */

  n = mtop->atomtypes.nr-1;


  /* Validate that this is indeed an inert hydrogen. */

  nvdwparam = fr->bBHAM ? 3 : 2;

  nti = nvdwparam * fr->ntype * n; /* The n-row */

  for (i=0; i<=n; i++)
    {
      vdwparam = fr->nbfp;
      ti = nti + nvdwparam * i; /* The offset, i-column */

      if (fr->bBHAM)
	{
	  /* Buckingham */
	  c[0]               = vdwparam[ti];
	  c[1]               = vdwparam[ti+1];
	  c[2]               = vdwparam[ti+2];
	  
	}
      else
	{
	  /* LJ */
	  c[0]               = vdwparam[ti];
	  c[1]               = vdwparam[ti+1];
	  c[2]               = 0.0;
	}

      if ((c[0] != 0.0) ||
	  (c[1] != 0.0) ||
	  (c[2] != 0.0))
	{
	  gmx_fatal(FARGS, "Atomtype %i is not inert!", n+1);
	}
    }

  return n;
}


extern void set_proton_presence(qhop_H_exist *Hext, const int atomid, const gmx_bool present)
{
  Hext->H[Hext->atomid2H[atomid]] = present ? (char)1: (char)0;
}

extern gmx_bool get_proton_presence(const qhop_H_exist *Hext, const int atomid)
{
  return Hext->H[Hext->atomid2H[atomid]] == (char)1;
}

extern void unindex_ilists(t_qhop_residue *qres)
{
  int i, j;
  
  /*Loop over bonded*/
  for (i=0; i<F_NRE; i++)
    {
	{
	  for (j=0; j < qres->bindex.nr[F_NRE]; j++)
	    {
	      qres->bindex.indexed[F_NRE] = FALSE;
	    }
	}
    }
  qres->nr_indexed = 0;
}

extern void index_ilists(t_qhop_residue *qres, const qhop_db *db,
			 const gmx_localtop_t *top, const t_commrec *cr)
{
  int b, i, ni, *ra, ia, ft, itype, nia, *l2g, bt, j;
  gmx_bool bMatch, bDD;

  const t_ilist *ilist = top->idef.il;;

  if (db->rb.bWater[qres->rtype])
    {
      return;
    }

/* #define ASSUME_ORDERED_BONDS */

  bDD = DOMAINDECOMP(cr);

  l2g = bDD ? cr->dd->gatindex : NULL; /* local to global atom translation */

  unindex_ilists(qres);

  for (bt=0; bt < ebtsNR; bt++)
    {

      itype = db->rb.btype[bt];
      if (itype <= 0)
	{
	  /* No bondeds of this type */
	  continue;
	}

      nia = interaction_function[itype].nratoms;

      i = 0;

      for (b=0; b < qres->bindex.nr[itype]; b++)
	{
	  /* get residue-local atom numbers */
	  ra = db->rb.ba[qres->rtype][bt][b];
	  
	  /* Loop the ilist */
#ifndef ASSUME_ORDERED_BONDS
	  i = 0; /* Start from the beginning for this next interaction. */
#endif
	  bMatch = FALSE;

	  while((i < ilist[itype].nr) && !bMatch)
	    {
	      ft = i++; /* functype/parameter index */
	      
	      bMatch = TRUE;
	      
	      for (j=0; j<nia; j++)
		{
		  /* Translate atoms in ilist to residue local numbers */
		  ia = bDD ?
		    l2g[ilist[itype].iatoms[i+j]] - qres->atoms[0] : /* local != global*/
		    ilist[itype].iatoms[i+j] - qres->atoms[0];       /* local != global*/

		  if (ia != ra[j])
		    {
		      /* No match. On to the next interaction; */
		      bMatch = FALSE;
		      break;
		    }
		}

	      if (!bMatch)
		{
		  /* Test the other order */

		  bMatch = TRUE;

		  for (j=0; j<nia; j++)
		    {
		      /* Translate atoms in ilist to residue local numbers */
		      ia = bDD ?
			l2g[ilist[itype].iatoms[i+j]] - qres->atoms[0] : /* local != global*/
			ilist[itype].iatoms[i+j] - qres->atoms[0];       /* local != global*/

		      if (ia != ra[nia-j-1])
			{
			  /* No match. On to the next interaction; */
			  bMatch = FALSE;
			  break;
			}
		    }

		}

	      if (!bMatch)
		{ 
		  i += (nia); /* proceed with next interaction */
		}
	      else
		{
		  /* Ok, we've found it */
		  break;
		}

	    } /* while */

	  if (bMatch)
	    {
	      /* interaction found. */
	      qres->bindex.ilist_pos[itype][b] = (t_iatom)i;
	      /* Should one store ft somewhere? */
	      qres->bindex.indexed[itype][b] = TRUE;
	      qres->nr_indexed++;
	    }
	  else
	    {
	      gmx_fatal(FARGS, "Interaction not found in ilist");
	    }


	} /* bonded loop*/

    }
}

/* returns the index in top->idef.il[?].iatom[] where bond to this proton is found. */
extern int qhop_get_proton_bond_params(const qhop_db *db, const t_qhoprec *qr, t_qhop_atom *qatom, gmx_localtop_t *top, int proton_id, const t_commrec *cr)
{
  int b, bi, i, ai;
  const int niatoms = 3;

  gmx_bool bMatch;

  t_restp *rtp;

  t_qhop_residue *qres;

  qres = &(qr->qhop_residues[qatom->qres_id]);

  rtp  = &(db->rtp[db->rb.res[qres->rtype][qres->res].rtp]);

  /* find the parameters for this constraint.
   * We assume that there's only one constraint
   * involving this hydrogen. This is NOT the case with
   * angle constraints. */
  for (b=0; b < rtp->rb[ebtsBONDS].nb; b++)
    {
      /* bi will index the list of bonded interactions of qres->rtype*/
      bi = db->rb.res[qres->rtype][qres->res].biMap[ebtsBONDS][b];

      for (i=0; i < (niatoms-1); i++)
	{
	  /* Assume global atom numbering. CHANGE!*/
	  ai = proton_id - qres->atoms[0]; /* residue local atom id of the proton. */
	  if (db->rb.ba[qres->rtype][ebtsBONDS][bi][i] == ai)
	    {
	      return db->rb.res[qres->rtype][qres->res].findex[ebtsBONDS][bi];
	    }
	}
    }
  gmx_fatal(FARGS, "Constraint not found!");
}

extern void qhop_constrain(t_qhop_residue *qres, t_qhoprec *qr, const qhop_db *db, gmx_localtop_t *top, int proton_id, const t_commrec *cr)
{
  int nr, nalloc, heavy, h, rt, r, b, bi, i, params, ai, *g2l, *iatoms;
  const int niatoms = 3;

  gmx_bool bMatch;

  char *heavyname, *hname;

  t_qhop_atom *qatom;

  t_restp *rtp;

  bMatch = FALSE;

  if (qres->nr_indexed == 0)
    {
      index_ilists(qres, db, top, cr);
    }

  for (heavy=0; (heavy < qres->nr_titrating_sites) && (!bMatch); heavy++)
    {
      qatom = &(qr->qhop_atoms[qres->titrating_sites[heavy]]);
      for (h=0; h < qatom->nr_protons; h++)
	{
	  if (qatom->protons[h] == proton_id)
	    {
	      heavyname = qatom->atomname;
	      heavy = qatom->atom_id;
	      bMatch = TRUE;
	      break;
	    }
	}
    }

  if (!bMatch)
    {
      /* doing nothing. The proton wasn't found on this residue. */
      return;
    }

  qhop_get_proton_bond_params(db, qr, qatom, top, proton_id, cr);
  
  if (DOMAINDECOMP(cr))
    {
      g2l = cr->dd->gatindex; /* use this to map global to local atomnumbers below */
    }

  /* Make a new entry at the end of the constraint ilist. */
  nr = top->idef.il[F_CONSTR].nr;
  nalloc = top->idef.il[F_CONSTR].nalloc;
  
  if (nr+niatoms > nalloc)
    {
      srenew(top->idef.il[F_CONSTR].iatoms, nalloc+niatoms);
      top->idef.il[F_CONSTR].nalloc += niatoms;
    }

  top->idef.il[F_CONSTR].iatoms[nr]   = params;
  top->idef.il[F_CONSTR].iatoms[nr+1] = DOMAINDECOMP(cr) ? g2l[heavy] : heavy;
  top->idef.il[F_CONSTR].iatoms[nr+2] = DOMAINDECOMP(cr) ? g2l[h]     : h;
  top->idef.il[F_CONSTR].nr += 3;

  /* Update bonded index */

  h = proton_id - qres->atoms[0]; /* Make residue local index */

  /* Now find the bonded term we're adding */
  for (b=0; b < qres->bindex.nr[F_CONSTR]; b++)
    {
      bi = db->rb.res[qres->rtype][qres->res].biMap[ebtsBONDS][b]; /* Map to restype bondeds */

      rtp = &(db->rtp[db->rb.res[qres->rtype][qres->res].rtp]);

      iatoms = db->rb.ba[qres->rtype][ebtsBONDS][bi];

      if (h == iatoms[0] || h == iatoms[1])
	{
	  qres->bindex.ilist_pos[F_CONSTR][b] = nr;
	  return;
	}
    }
  gmx_fatal(FARGS, "Could not find the constraint in the rtp data.");
}

extern void qhop_deconstrain(t_qhop_residue *qres, const qhop_db *db, gmx_localtop_t *top, int proton_id, const t_commrec *cr)
{
  int b, ip, *iatoms;
  const int niatoms = 3;

  if (qres->nr_indexed == 0)
    {
      index_ilists(qres, db, top, cr);
    }

  for (b=0; b < qres->bindex.nr[F_CONSTR]; b++)
    {
      ip = qres->bindex.ilist_pos[F_CONSTR][b];

      if (ip == -1)
	{
	  /* Can't remove this constraint, it's not really indexed
	   * which probably means it's not there. */
	  return;
	}

      iatoms = &(top->idef.il[F_CONSTR].iatoms[ip]);

      if (iatoms[1] == proton_id || iatoms[2] == proton_id)
	{
	  /* Here it is. Remove it from the list. */

	  /* Here the ilist is shifted by niatoms to erase the constraint in question. */
	  memmove((void*)&(iatoms[0]),
		  (void*)&(iatoms[niatoms]),
		  top->idef.il[F_CONSTR].nr-ip-niatoms);

	  top->idef.il[F_CONSTR].nr -= niatoms;

	  qres->bindex.ilist_pos[F_CONSTR][b] = -1;

	  break;
	}
    }  
}


/* Reads the info in the db->qhop_H_exist and finds the corresponding
 * residue subtype in the qhop_db.
 * Returns the index in db->res[rt][]
 * This function is used to set the correct protonation states
 * at the start of the simulation.
 * Right now matching is based on counting the protons on
 * titrating sites in the qhop_res and in the rtp. Thus,
 * the existence map may need slight reshuffling to really
 * represent the global protonation state. */
extern int which_subRes(const gmx_mtop_t *top, const t_qhoprec *qr,
			qhop_db *db, const int resnr)
{
  int
    h, i, j, k, t, nH, *nHres, a, r, b, aa,
    atomid, nres, natoms, qrtp;
  char
    *Hname, *DAname;
  t_atom
    atom;
  t_qhop_residue
    *qres;
  gmx_bool
    bSameRes, bNew, bMatch;
  char
    **DAlist, **DAlist2;
  int
    *n_H, *n_H2, nDA, nDA2, DA;
  qhop_reactant
    *reac;
  t_restp
    *rtp;

  qres = &(qr->qhop_residues[resnr]);
  nres = db->rb.nres[qres->rtype];


  /* To better handle the tautomerism, maybe one should
     count hydrogens on the donors/acceptors instead
     of matching the names. Here we match the names.
  */

  nDA = qres->nr_titrating_sites;
  snew(n_H, nDA);
  snew(DAlist, nDA);

  /* ************************************************
     Count the protons present on each titrating site
     (qhop_atom) for this qhop_residue.
  */

  /* loop over donors/acceptors in the residue */
  for (i=0; i < nDA; i++)
    {
      k = qres->titrating_sites[i];

      /* Stash the donor/acceptor name */
      DAlist[i] = qr->qhop_atoms[k].atomname;

      /* Proton roll call */
      for (j=0; j < qr->qhop_atoms[k].nr_protons; j++)
	{
	  /* get the atom id of the protons */
	  h = qr->qhop_atoms[k].protons[j];

	  /* is it present? */
	  if (get_proton_presence(&(db->H_map), h))
	    {
	      n_H[i]++;
	    }
	}
    }

  /* Ok. n_H now stores the number of active protons on all donors/acceptors.
     Donor/acceptor names are stored in DAlist. */


  /* ********************************************************
     Count the protons on each titrating site for different
     residue subtypes and see if any matches the qhop_residue
  */

  /* Loop over residue subtypes */
  bSameRes = FALSE;
  for (r=0; r<nres && !bSameRes; r++)
    {
      n_H2 = NULL;
      DAlist2 = NULL;
      nDA2 = 0;

      bMatch = TRUE;

      /* DA==0 => acceptors, DA==1 => donors */
      for (DA=0; DA<2 && bMatch; DA++)
	{
	  /* Go through the donors and acceptors and make a list */
	  for (j=0;
	       (j < (DA==0 ?
		     db->rb.res[qres->rtype][r].na :
		     db->rb.res[qres->rtype][r].nd))
		 && bMatch;
	       j++)
	    {
	      reac = (DA==0 ?
		      &(db->rb.res[qres->rtype][r].acc[j]) :
		      &(db->rb.res[qres->rtype][r].don[j]));

	      /* Tautomer loop. There may be chemically equivalent
	       * sites, e.g. the oxygens in a carboxylate. */
	      for (t=0; t<reac->nname && bMatch; t++)
		{
		  /* Have we already seen this donor/acceptor? */
		  bNew = TRUE;
		  for (k=0; (k < nDA2) && bNew; k++)
		    {
		      if (strcmp(DAlist2[k], reac->name[t]) == 0)
			{
			  /* We already had it in the list */
			  bNew = FALSE;
			}
		    }
		  
		  if (bNew)
		    {
		      srenew(DAlist2, nDA2+1);
		      srenew(n_H2, nDA2+1);

		      n_H2[nDA2] = 0;
		      DAlist2[nDA2] = reac->name[t];
		      
		      rtp = &(db->rtp[db->rb.res[qres->rtype][r].rtp]);
		      for (a=0; a<rtp->natom; a++)
			{
			  if (strcmp(reac->name[t], *(rtp->atomname[a])) == 0)
			    {
			      /* Here's the titrating site. Find hydrogens.
			       * Assume a contiguous strtch of hydrogens followed
			       * by the next heavy atom. */
			      for (aa=a+1; aa<rtp->natom; aa++)
				{
				  if (*(rtp->atomname[aa]) && *(rtp->atomname[aa][0]) == 'H')
				    {
				      /* A(nother) hydrogen. */
				      n_H2[nDA2]++;
				    }
				  else
				    {
				      /* Next heavy atom. */
				      break;
				    }
				}
			      break;
			    }
			}


		      /* How do we know the number of protons? We'll have to scan the rtp. */
/* 		      for (b=0; b < db->rtp[reac->rtp].rb[ebtsBONDS].nb; b++) */
/* 			{ */
/* 			  rtp = &(db->rtp[reac->rtp]); */
/* 			  n_H2[nDA2] = 0; */
/* 			  for (a=0; a<2; a++) */
/* 			    { */
/* 			      /\* If the bond is between the donor and a H* then we ++ the hydrogen count *\/ */
/* 			      DAname = rtp->rb[ebtsBONDS].b[b].a[a%2]; */
/* 			      Hname  = rtp->rb[ebtsBONDS].b[b].a[(a+1)%2]; */

/* 			      if ((strcmp(DAname, DAlist2[nDA2]) == 0) && */
/* 				  (strncmp(Hname, "H", 1) == 0)) */
/* 				{ */
/* 				  /\* Yes, this H was connected to this donor/acceptor *\/ */
/* 				  n_H2[nDA2]++; */
/* 				} */
/* 			    } */
/* 			} */


/* 		      /\* Is the proton count the same for the titrating_sites[] in qhop_residue? *\/ */
/* 		      bMatch = FALSE; */
/* 		      for (i=0; i<nDA; i++) */
/* 			{ */
/* 			  if (strcmp(DAlist2[nDA2], DAlist[i]) == 0) */
/* 			    { */
/* 			      /\* Same donor/acceptor *\/ */
/* 			      if (n_H2[nDA2] == n_H[i]) */
/* 				{ */
/* 				  /\* Same proton count *\/ */
/* 				  bMatch = TRUE; */
/* 				} */
/* 			    } */
/* 			} */

/* 		      if (!bMatch) */
/* 			{ */
/* 			  break; */
/* 			} */

		      nDA2++;
		    }
		}
	    }
	}

      /* Now compare the hydrogen counts to see if this is the flavor we want. */
      /* First check if the total number of hydrogens are the same for both lists */
      nH = 0;
      for (j=0; j<nDA; j++)
	{
	  nH += n_H[j];
	}

      for (j=0; j<nDA2; j++)
	{
	  nH -= n_H2[j];
	}
      
      bSameRes = (nH==0); /* Same total number of hydrogens? */

      /* Sum up hydrogens on tautomeric sites and compare */

      /* DA==0 => acceptors, DA==1 => donors */
      for (DA=0; DA<2 && bSameRes; DA++)
	{
	  for (j=0;
	       (j < (DA==0 ?
		     db->rb.res[qres->rtype][r].na :
		     db->rb.res[qres->rtype][r].nd))
		 && bSameRes;
	       j++)
	    {
	      reac = (DA==0 ?
		      &(db->rb.res[qres->rtype][r].acc[j]) :
		      &(db->rb.res[qres->rtype][r].don[j]));

	      nH = 0;
	      
	      for (t=0; t<reac->nname && bSameRes; t++)
		{
		  for (i=0; i<nDA; i++)
		    {
		      if (strcmp(DAlist[i], reac->name[t]) == 0)
			{
			  nH += n_H[i];
			  break;
			}
		    }

		  for (i=0; i<nDA2; i++)
		    {
		      if (strcmp(DAlist2[i], reac->name[t]) == 0)
			{
			  nH -= n_H2[i];
			  break;
			}
		    }

		  /* If they are the same, then nH should be zero here. */
		  bSameRes = (nH==0);
		}
	    }
	}

      if (bSameRes)
	{
	  /* Adjust the existence map */
	  
	  /* Zero all hydrogens */
	  for (i=0; i < qres->nr_titrating_sites; i++)
	    {
	      for (j=0; j < qr->qhop_atoms[qres->titrating_sites[i]].nr_protons; j++)
		{
		  set_proton_presence(&(db->H_map),
				      qr->qhop_atoms[qres->titrating_sites[i]].protons[j],
					 FALSE);
		}
	    }
	  
	  /* Go through the rtp */
	  rtp = &(db->rtp[db->rb.res[qres->rtype][r].rtp]);
	  
	  for (i=0; i < rtp->natom; i++)
	    {
	      /* Is it a hydrogen atom? */
	      if (rtp->atom[i].atomnumber == 1)
		{
		  /* Match with atoms in qres */
		  for (j=0; j < qres->nr_atoms; j++)
		    {
		      if (strcmp(*(rtp->atomname[i]), qres->atomnames[j]) == 0)
			{
			  set_proton_presence(&(db->H_map),
					      qres->atoms[j],
					      TRUE);
			}
		    }
		}
	    }
	}


      /* Clean up */
      if (n_H2 != NULL)
	{
	  sfree(n_H2);
	}
      
      if (DAlist2 != NULL)
	{
	  sfree(DAlist2);
	}
      
      if (bSameRes)
	{
	  break;
	}
    }

  if (r >= nres)
    {
      gmx_fatal(FARGS, "Didn't find the subres.");
    }

  return r;

 
  /* Here's the other method, matching H-names exactly to the
     residue subtypes. Makes gromacs a bit too strict perhaps. */

  /* Make a list of all hydrogens for this restype */
/*   qrtp = db->rb.rtp[qres->rtype]; /\* The rtp data *\/ */
/*   natoms = db->rtp[qrtp].natom; */

/*   for (i=0, nH=0; */
/*        i < natoms; */
/*        i++) */
/*     { */
/*       if (db->rtp[qrtp].atom[i].atomnumber == 1) */
/* 	{ */
/* 	  srenew(Hlist, nH+1); */
/* 	  Hlist[nH] = *(db->rtp[qrtp].atomname[i]); */
/* 	  nH++; */
/* 	} */
/*     } */

  /* Loop over the residue subtypes and see which one matches */
  /* for (r=0; r<nres; r++) */
/*     { */
/*       /\* scan the rtp *\/ */
/*       for (j=0; j < db->nrtp; j++) */
/* 	{ */
/* 	  if (gmx_strcasecmp(db->rtp[j].resname, */
/* 			     db->rb.res[qres->rtype][r]) == 0) */
/* 	    { /\* Mathcing resnames! Now look at the protons. *\/ */
/* 	      bSameRes = TRUE; */
/* 	    } */
/* 	} */
/*     } */
}

extern void qhop_swap_bondeds(t_qhop_residue *swapres, qhop_res *prod)
{
  /* placeholder. */
  int i;
  i=0;
  
}

/* We change vdv by changing atomtype. */
extern void qhop_swap_vdws(const t_qhop_residue *swapres,
			   const qhop_res *prod,
			   t_mdatoms *md,
			   const qhop_db *db)
{

  int i, j;
  t_restp *rtp;

/*   if (db->rb.bWater[swapres->rtype]) */
/*       {return;} */
  
  rtp = &(db->rtp[prod->rtp]);

  /* For now, assume that all atomtypes are present, e.g. that grompp was run with -norenum */

  /* Just make it all inert... */
  for (i=0; i < swapres->nr_atoms; i++)
    {
      md->typeA[swapres->atoms[i]] = db->inertH;
    }

  /* ...then set the atomtypes for the atoms in the residue */
  for (i=0; i < prod->niatom; i++)
    {
      md->typeA[swapres->atoms[prod->iatomMap[i]]] = rtp->atom[i].type;
    }
}

static void low_level_swap_m_and_q(t_mdatoms *md, const t_atom *atom, const int atomid)
{
  real m, q;
  
  if (atom == NULL)
    {
      m = 0;
      q = 0;
    }
  else
    {
      m = atom->m;
      q = atom->q;
    }

  md->chargeA[atomid] = q;

  if (md->massA != NULL)
    {
      md->massA[atomid] = m;
    }

  md->invmass[atomid] = (m==0 ? 0 : 1/m);
  
}

/* Hacks mdatoms such that the masses and
 * charges of swapres match those of prod.
 * Works on an atomname basis
 */
extern void qhop_swap_m_and_q(const t_qhop_residue *swapres,
			      const qhop_res *prod,
			      t_mdatoms *md,
			      const qhop_db *db, t_qhoprec *qr)
{
  int i, j, nH;
  t_restp *rtp;
  real m;
  gmx_bool bWater;
  t_qhop_atom *qa;

  bWater = db->rb.bWater[swapres->rtype];
  
  rtp = &(db->rtp[prod->rtp]);

  /* Compare atomnames with atomnames in rtp.
   * Change m and q upon match. */

  for (i=0; i < swapres->nr_atoms; i++)
    {
      /* Zero m and q for all atoms
       * Otherwise any atoms that was
       * just turned off will retain
       * their m and q. */

      low_level_swap_m_and_q(md, NULL, swapres->atoms[i]);

      for (j=0; j < rtp->natom; j++)
	{
	  if (strcmp(*(rtp->atomname[j]), swapres->atomnames[i]) == 0)
	    {
	      /* Now set the q and m for real */
	      low_level_swap_m_and_q(md, &(rtp->atom[j]), swapres->atoms[i]);
	      break;
	    }
	}
    }

  if (bWater)
    {
      /* Masses are different for water. */

      qa = &(qr->qhop_atoms[swapres->titrating_sites[0]]);
      nH = 0;

      for (i=0; i<qa->nr_protons; i++)
	{
	  if (get_proton_presence(&db->H_map, qa->protons[i]))
	    {
	      nH++;
	    }
	}

      /* Hardcoded = ugly. Sorry. */
      if (md->massA != NULL)
      {
	md->massA[qa->protons[0]] = 1.008;
	md->massA[qa->protons[1]] = 1.008;
	md->massA[qa->protons[2]] = 0; /* vsite */
	md->massA[qa->protons[3]] = 0; /* vsite */
	md->massA[qa->protons[4]] = 0; /* vsite */
	md->massA[qa->atom_id]    = 15.9994 + (nH-2)*1.008;
      }

      md->invmass[qa->protons[0]] = 1/1.008;
      md->invmass[qa->protons[1]] = 1/1.008;
      md->invmass[qa->protons[2]] = 0; /* vsite */
      md->invmass[qa->protons[3]] = 0; /* vsite */
      md->invmass[qa->protons[4]] = 0; /* vsite */
      md->invmass[qa->atom_id]    = 1 / (15.9994 + (nH-2)*1.008);

    }

}

extern void set_interactions(t_qhoprec *qr, const qhop_db *qdb, t_mdatoms *md, t_qhop_atom *QA, t_qhop_residue *qres)
{
  int i, j, k, nri, bt, b, Gr, RB, R;
  qhop_res *reac;
  t_restp *res;
  
  /* point pointers at the right residue */
  Gr   = qres->res_nr; /* global res id. */
  RB   = qres->rtype;
  R    = qres->res;
  reac = &(qdb->rb.res[RB][R]);

  for (i=0; i<qres->nr_titrating_sites; i++)
    {
      /* find among qdb->res[][].acc/.don */
      for (j=0; j < reac->na; j++)
	{
	  /* loop over tautomeric sites */
	  for (k=0; k < reac->acc[j].nname; k++)
	    {
	      if (strcmp(reac->acc[j].name[k], QA[qres->titrating_sites[i]].atomname) == 0)
		{
		  QA[qres->titrating_sites[i]].state |= eQACC;
		  break;
		}
	    }
	}

      /* find among qdb->res[][].acc/.don */
      for (j=0; j<reac->nd; j++)
	{
	  /* loop over tautomeric sites */
	  for (k=0; k<reac->don[j].nname; k++)
	    {
	      if (strcmp(reac->don[j].name[k], QA[qres->titrating_sites[i]].atomname) == 0)
		{
		  QA[qres->titrating_sites[i]].state |= eQDON;
		  break;
		}
	    }
	}
    }

  qhop_swap_bondeds(qres, reac);
  qhop_swap_vdws(qres, reac, md, qdb);
  qhop_swap_m_and_q(qres, reac, md, qdb, qr);
  
    /* Non-bonded */
}

/* Sets the bqhopdonor[] and bqhopacceptor[] arrays in a t_mdatoms. */
extern void qhop_atoms2md(t_mdatoms *md, const t_qhoprec *qr)
{
  int i;
  t_qhop_atom *a;

  for (i=0; i < qr->nr_qhop_atoms; i++)
    {
      a = &(qr->qhop_atoms[i]);

      md->bqhopacceptor[a->atom_id] = (a->state & eQACC) != 0;
      md->bqhopdonor[a->atom_id]    = (a->state & eQDON) != 0;
    }
}
