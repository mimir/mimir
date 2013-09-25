from django.db import models
from django.contrib.auth.models import User

# Create your models here.

class Reference(models.Model):
    name = models.CharField(max_length = 200, unique = True) #Name of original tutorial
    author = models.CharField(max_length = 100)
    url = models.CharField(max_length = 300)
    def __unicode__(self):
        return self.name

class Course(models.Model):
    name = models.CharField(max_length = 100, unique = True)
    description = models.TextField()
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    def __unicode__(self):
        return self.name
        
class Lesson(models.Model):
    name = models.CharField(max_length = 100, unique = True)
    url = models.CharField(max_length = 100, unique = True)
    tutorial = models.TextField()
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    description = models.CharField(max_length = 400)
    course = models.ForeignKey(Course)
    @property
    def times_taken(self):
        return self.usertakeslesson_set.count() #TODO ensure this is efficient

    def __unicode__(self):
        return self.name

class LessonReferencesReference(models.Model):
    lesson = models.ForeignKey(Lesson)
    reference = models.ForeignKey(Reference)
    def __unicode__(self):
        return self.lesson.name + " references " + self.reference.name

class Example(models.Model):
    lesson = models.ForeignKey(Lesson)
    problem = models.TextField()
    solution = models.TextField()
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    def __unicode__(self):
        return self.problem

class AnswerFormat(models.Model):
    name = models.CharField(max_length = 100)
    hint = models.CharField(max_length = 250)
    example = models.CharField(max_length = 250)
    regex = models.CharField(max_length = 300)
    def __unicode__(self):
        return self.name

class Question(models.Model):
    lesson = models.ForeignKey(Lesson)
    question = models.CharField(max_length = 300)
    variables = models.TextField() #TODO: Move variable declarations from the template to this field
    answer = models.CharField(max_length = 200) #Use this field as the input to the MAS
    answer_format = models.ForeignKey(AnswerFormat)
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    @property
    def times_answered(self):
        return self.useranswersquestion_set.count() #TODO ensure this is efficient
    #TODO add other useful queries here e.g. times answered correctly
    def __unicode__(self):
        return self.question
		
class LessonFollowsFromLesson(models.Model):
    leads_from = models.ForeignKey(Lesson, related_name='supplement')
    leads_to = models.ForeignKey(Lesson, related_name='preparation') #TODO think about these two related names, do they make sense?
    CHOICES = [(i,i) for i in range(11)] #0-10 #TODO potentially remove the link strength now that lessons strictly follow on from each other
    strength = models.PositiveSmallIntegerField(choices = CHOICES) #Not sure exactly how this will work yet TODO work it out. Roughly 10 should correspond to, you really need to know A to do B, and 0 should mean that they're sort of linked?

    class Meta:
        unique_together = ("leads_to", "leads_from")
        ordering = ['-strength']

    def __unicode__(self):
        return self.leads_from.name + " leads to " + self.leads_to.name

class Topic(models.Model):
    name = models.CharField(max_length = 100, unique = True)
    description = models.TextField()
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    def __unicode__(self):
        return self.name

class CourseIsOnTopic(models.Model):
    course = models.ForeignKey(Course)
    topic = models.ForeignKey(Topic)
    def __unicode__(self):
        return self.course.name + " is on " + self.topic.name
