.PHONY: commit package release clean

.DEFAULT_GOAL := commit

POWERSHELL := powershell -NoProfile -ExecutionPolicy Bypass

commit:
	@$(POWERSHELL) -File scripts\package.ps1 -Commit

package:
	@$(POWERSHELL) -File scripts\package.ps1

release:
	@$(POWERSHELL) -File scripts\package.ps1 -Release

clean:
	@$(POWERSHELL) -File scripts\package.ps1 -Clean
