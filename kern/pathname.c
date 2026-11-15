// PATHNAMES
#include "defs.h"
#include "malloc.h"
#include "proc.h"
#include "inc/dir.h"
#include "pathname.h"

int pn_get(uintptr_t upath, struct pathname *pn) {
  int error;

  pn->pn_buf = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);
  if (upath == 0)
    error = FNU_ERR_FAULT;
  else
    error = copyinstr(curproc->p_p->pgdir, pn->pn_buf, upath, MAXPATHLEN,
                      &pn->pn_len);
  if (error != 0) {
    free(pn->pn_buf, M_TEMP);
    pn->pn_buf = 0;
  }
  return error;
}

int pn_get_maybe(uintptr_t upath, struct pathname *pn) {
  if (upath == 0) {
    pn->pn_buf = 0;
    pn->pn_len = 0;
    return 0;
  }
  return pn_get(upath, pn);
}

void pn_free(struct pathname *pn) {
  if (pn->pn_buf != 0)
    free(pn->pn_buf, M_TEMP);
  pn->pn_buf = 0;
}
