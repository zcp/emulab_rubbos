#!/bin/sh

# CJDBC_HOME definition
if [ -z "$CJDBC_HOME" ] ; then 
  echo "Cannot find C-JDBC home directory. Please set the CJDBC_HOME environment variable."
  exit 1
fi

# JAVA setup.
#
# We try to use first the java JVM in JAVA_HOME and if not found,
# we use the one found in the path.
# You can specify additional options to give to the Java JVM in the
# JAVA_OPTS environment variable.
if [ -z "$JAVA_HOME" ]
then
    JAVA=`which java`
    if [ ! -x "$JAVA" ]; then
      echo "Cannot find java command. Please set the JAVA_HOME environment variable."
      exit 1
    fi
else
    JAVA=$JAVA_HOME/bin/java
fi
export JAVA
  CJDBC_DEFAULT_JVMDEBUG_PORT=53001
  
# uncomment to debug
# JVM_OPTIONS="$JVM_OPTIONS -enableassertions -Xdebug -Xnoagent -Djava.compiler=none -Xrunjdwp:transport=dt_socket,server=y,suspend=n,address=$CJDBC_DEFAULT_JVMDEBUG_PORT"
  
# Set the classpath.
unset CLASSPATH
CLASSPATH=:$CJDBC_HOME/lib/metouia.jar:$CJDBC_HOME/doc/userGuide/html:$CJDBC_HOME/xml:$CJDBC_HOME/lib/crimson.jar:$CJDBC_HOME/lib/jline.jar:$CJDBC_HOME/config/language:$CJDBC_HOME/lib/c-jdbc-console.jar:$CJDBC_HOME/lib/jcommon.jar:$CJDBC_HOME/lib/jfreechart.jar:$CJDBC_HOME/drivers/c-jdbc-driver.jar:$CJDBC_HOME/lib/commons-cli.jar:$CJDBC_HOME/lib/jmx/mx4j.jar:$CJDBC_HOME/lib/jmx/mx4j-remote.jar:$CJDBC_HOME/lib/jmx/mx4j-tools.jar:$CJDBC_HOME/lib/log4j.jar:$CJDBC_HOME/lib/dom4j-1.5-beta-2.jar:$CJDBC_HOME/lib/jaxen-1.1-beta-2.jar

# Change directory if needed


# Java Command.
$JAVA $JVM_OPTIONS -classpath $CLASSPATH  -Dswing.defaultlaf=net.sourceforge.mlf.metouia.MetouiaLookAndFeel org.objectweb.cjdbc.console.text.ConsoleLauncher -t  "$@"
