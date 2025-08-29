# Tim's Silly Little System Sidecar

I needed something small that kept an eye on:

 - CPU usage
 - RAM usage (swap seaprately)
 - iowait over time

I'm an avid `btop` user and love the high-bit ascii graphs, but
it's too big for what I wanted, which was essentially a little "side car"
I could use in a terminal to watch what happened while I tweaked and loaded
language models (mostly while working on [Runa][1] components). 

It .. took on a life of its own, as these things tend to do, and I added 
one argument that lets you watch a file `tail -f` style, and then added additional
code to also parse and display:

 - System loads (1, 5, 15 minute)
 - Processes (running / total)
 - Power (batt level or AC) <- essential for field coding sessions :)

## Building

It has no dependencies aside from glibc/libc, not even ncurses (ANSI-based).

Edit sidecar.c and change any constants you want to tweak, then:

```sh
> make
```

Finally, (if desired), copy the compiled `sidecar` binary somewhere in your path.

## Usage

```sh
> ./sidecar [optional_file_to_watch]
```

## Screenshot

![Sidecar Screenshot](https://raw.githubusercontent.com/timthepost/sidecar/refs/heads/main/assets/sidecar_screenshot.png "Sidecar While Warming Up A Model")

### TODO List

 - Use arguments to override constants
 - Maybe colorize the output? 

Questions? Kvetches? Thoughts about the universe or what you ate for lunch? `<timthepost@protonmail.com>`.

Or, send a PR (if you can). 
