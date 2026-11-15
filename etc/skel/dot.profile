# .profile - read by sh(1) for a login shell, after /etc/profile.
# What is set here is yours; /etc/profile is everyone's.

if [ -d "$HOME/bin" ]; then
	PATH=$PATH:$HOME/bin
	export PATH
fi
