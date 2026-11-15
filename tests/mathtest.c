#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static void near(const char *what, double got, double want) {
  double err = fabs(got - want), tol = 4 * DBL_EPSILON * (fabs(want) > 1 ? fabs(want) : 1);
  int good = err <= tol;

  printf("mathtest: %-24s %s\n", what, good ? "ok" : "FAILED");
  if (!good) {
    printf("mathtest:   got %.17g want %.17g\n", got, want);
    failures++;
  }
}

int main(void) {
  double ip;

  near("sqrt(2)", sqrt(2.0), 1.4142135623730951);
  near("sin(1)", sin(1.0), 0.8414709848078965);
  near("cos(1)", cos(1.0), 0.5403023058681398);
  near("sin(100)", sin(100.0), -0.5063656411097588);
  near("tan(0.5)", tan(0.5), 0.5463024898437905);
  near("atan2(1,-1)", atan2(1.0, -1.0), 2.356194490192345);
  near("exp(1)", exp(1.0), 2.718281828459045);
  near("log(10)", log(10.0), 2.302585092994046);
  near("log10(2)", log10(2.0), 0.3010299956639812);
  near("pow(2,0.5)", pow(2.0, 0.5), 1.4142135623730951);
  near("pow(10,-3)", pow(10.0, -3.0), 0.001);
  near("fmod(10,3)", fmod(10.0, 3.0), 1.0);
  near("modf(3.75)", modf(3.75, &ip), 0.75);
  near("modf integral", ip, 3.0);
  near("floor(-2.5)", floor(-2.5), -3.0);
  near("ceil(-2.5)", ceil(-2.5), -2.0);
  near("hypot(3,4)", hypot(3.0, 4.0), 5.0);
  near("cbrt(27)", cbrt(27.0), 3.0);
  near("lgamma(5)", lgamma(5.0), 3.1780538303479458);
  near("strtod 1e-300", strtod("1e-300", 0) * 1e300, 1.0);
  printf("mathtest: %-24s %s\n", "isnan(sqrt(-1))", isnan(sqrt(-1.0)) ? "ok" : "FAILED");
  failures += !isnan(sqrt(-1.0));
  printf(failures ? "mathtest: FAILED\n" : "mathtest: ok\n");
  return failures != 0;
}
